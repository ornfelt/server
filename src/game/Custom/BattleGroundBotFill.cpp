/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#if defined(USE_CUSTOM_CHANGES) && defined(ENABLE_PLAYERBOTS)

#include "Custom/BattleGroundBotFill.h"
#include "BattleGroundMgr.h"
#include "Database/DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "playerbot.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace
{
    uint32 const UPDATE_INTERVAL        = 1000;     // ms
    uint32 const MAX_ADDS_PER_UPDATE    = 10;
    uint32 const MAX_PREPARES_PER_UPDATE = 3;       // the level-up of a bot is the expensive step

    time_t const QUEUE_JOIN_GRACE       = 10;       // s, a queue join reaches the player's queue data
    time_t const LOGIN_TIMEOUT          = 60;       // s
    time_t const LEAVE_TIMEOUT          = 30;       // s, for the port out of a match
    time_t const ACCEPT_DELAY           = 2;        // s after the invitation, as a player who reads it

    uint32 const SPELL_DESERTER         = 26013;
    uint8 const PORT_ENTER              = 1;        // CMSG_BATTLEFIELD_PORT action: enter the battle
}

BattleGroundBotFill::BattleGroundBotFill() : m_updateTimer(0)
{
}

void BattleGroundBotFill::OnQueueJoin(Player* player, BattleGroundQueueTypeId queueTypeId, BattleGroundTypeId bgTypeId)
{
    if (!player || !sPlayerbotAIConfig.enabled || !IsRealPlayer(player))
    {
        return;
    }
    if (!sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId))
    {
        return;
    }

    FillRequest request;
    request.key.queueTypeId = queueTypeId;
    request.key.bgTypeId = bgTypeId;
    request.key.bracketId = player->GetBattleGroundBracketIdFromLevel(bgTypeId);
    request.level = player->getLevel();
    request.joinTime = time(NULL);
    m_requests[std::make_pair(player->GetGUIDLow(), queueTypeId)] = request;

    sLog.outString("BattleGroundBotFill: %s (level %u) queued for battleground type %u, filling the queue with bots",
                   player->GetName(), request.level, uint32(bgTypeId));
}

void BattleGroundBotFill::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.enabled)
    {
        return;
    }

    m_updateTimer += diff;
    if (m_updateTimer < UPDATE_INTERVAL)
    {
        return;
    }
    m_updateTimer = 0;

    time_t now = time(NULL);
    KeyLevelMap waitingKeys;
    std::map<FillKey, bool> activeKeys;
    UpdateRequests(now, waitingKeys, activeKeys);

    for (KeyLevelMap::const_iterator itr = waitingKeys.begin(); itr != waitingKeys.end(); ++itr)
    {
        FillQueue(itr->first, itr->second, now);
    }

    UpdateBots(now, activeKeys);
}

void BattleGroundBotFill::UpdateRequests(time_t now, KeyLevelMap& waitingKeys, std::map<FillKey, bool>& activeKeys)
{
    for (std::map<std::pair<uint32, BattleGroundQueueTypeId>, FillRequest>::iterator itr = m_requests.begin(); itr != m_requests.end();)
    {
        Player* owner = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, itr->first.first), false);
        BattleGroundQueueTypeId queueTypeId = itr->first.second;
        FillRequest const& request = itr->second;

        bool inQueue = owner && owner->InBattleGroundQueueForBattleGroundQueueType(queueTypeId);
        bool joining = owner && !inQueue && now - request.joinTime < QUEUE_JOIN_GRACE;
        if (!inQueue && !joining)
        {
            // left the queue or its match: the bots follow once their own match is over
            m_requests.erase(itr++);
            continue;
        }

        activeKeys[request.key] = true;
        if (joining || IsWaitingInQueue(owner, queueTypeId))
        {
            waitingKeys[request.key] = request.level;
        }
        ++itr;
    }
}

void BattleGroundBotFill::FillQueue(FillKey const& key, uint32 level, time_t now)
{
    BattleGround* bgTemplate = sBattleGroundMgr.GetBattleGroundTemplate(key.bgTypeId);
    if (!bgTemplate)
    {
        return;
    }

    // the minimum is what pops the queue; more would only add bots that stand around
    uint32 teamSize = std::max<uint32>(1, bgTemplate->GetMinPlayersPerTeam());
    uint32 adds = 0;
    for (int i = 0; i < 2; ++i)
    {
        Team team = i == 0 ? ALLIANCE : HORDE;
        uint32 count = CountTeam(key, team);
        uint32 added = 0;
        for (; count + added < teamSize && adds < MAX_ADDS_PER_UPDATE; ++added, ++adds)
        {
            if (!AddOnlineBot(key, team, level, now) && !AddOfflineBot(key, team, level, now))
            {
                break;
            }
        }
        if (added)
        {
            sLog.outString("BattleGroundBotFill: battleground type %u - %s %u of %u, adding %u bots",
                           uint32(key.bgTypeId), team == ALLIANCE ? "alliance" : "horde", count, teamSize, added);
        }
        else if (count < teamSize && adds < MAX_ADDS_PER_UPDATE)
        {
            sLog.outDetail("BattleGroundBotFill: no free %s random bot for battleground type %u (%u of %u)",
                           team == ALLIANCE ? "alliance" : "horde", uint32(key.bgTypeId), count, teamSize);
        }
    }
}

uint32 BattleGroundBotFill::CountTeam(FillKey const& key, Team team) const
{
    uint32 count = 0;

    // own bots of this key that are not in a match yet
    for (std::map<uint32, FillBot>::const_iterator itr = m_bots.begin(); itr != m_bots.end(); ++itr)
    {
        if (!(itr->second.key == key) || itr->second.team != team || itr->second.state == BotState::LEAVING)
        {
            continue;
        }
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(itr->first);
        if (!bot || !bot->InBattleGround())
        {
            ++count;
        }
    }

    // the requesting real players of this key and team (waiting, or on the way into the queue)
    for (std::map<std::pair<uint32, BattleGroundQueueTypeId>, FillRequest>::const_iterator itr = m_requests.begin(); itr != m_requests.end(); ++itr)
    {
        if (!(itr->second.key == key))
        {
            continue;
        }
        Player* owner = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, itr->first.first), false);
        if (owner && owner->GetTeam() == team && !owner->InBattleGround())
        {
            ++count;
        }
    }
    return count;
}

bool BattleGroundBotFill::AddOnlineBot(FillKey const& key, Team team, uint32 level, time_t now)
{
    // an online random bot of the team, free (no group, queue or battleground, not in combat); one at
    // the wanted level skips the level-up
    std::vector<Player*> sameLevel;
    std::vector<Player*> otherLevel;
    for (PlayerBotMap::const_iterator itr = sRandomPlayerbotMgr.GetPlayerBotsBegin(); itr != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++itr)
    {
        Player* bot = itr->second;
        if (!bot || bot->GetTeam() != team || m_bots.count(bot->GetGUIDLow()) || !IsFree(bot))
        {
            continue;
        }
        (bot->getLevel() == level ? sameLevel : otherLevel).push_back(bot);
    }

    std::vector<Player*> const& candidates = sameLevel.empty() ? otherLevel : sameLevel;
    if (candidates.empty())
    {
        return false;
    }

    Player* bot = candidates[urand(0, candidates.size() - 1)];
    FillBot& fillBot = m_bots[bot->GetGUIDLow()];
    fillBot.key = key;
    fillBot.team = team;
    fillBot.level = level;
    fillBot.state = BotState::PREPARING;
    fillBot.stateTime = now;
    fillBot.loggedInByFill = false;
    fillBot.entered = false;
    return true;
}

bool BattleGroundBotFill::AddOfflineBot(FillKey const& key, Team team, uint32 level, time_t now)
{
    if (sPlayerbotAIConfig.randomBotAccounts.empty())
    {
        return false;
    }

    std::ostringstream accounts;
    for (std::list<uint32>::const_iterator itr = sPlayerbotAIConfig.randomBotAccounts.begin(); itr != sPlayerbotAIConfig.randomBotAccounts.end(); ++itr)
    {
        accounts << (itr == sPlayerbotAIConfig.randomBotAccounts.begin() ? "" : ",") << *itr;
    }

    // the highest offline characters first: the fewer levels to give, the cheaper the bot
    QueryResult* result = CharacterDatabase.PQuery(
        "SELECT `guid`, `race` FROM `characters` WHERE `online` = 0 AND `account` IN (%s) ORDER BY `level` DESC LIMIT 200",
        accounts.str().c_str());
    if (!result)
    {
        return false;
    }

    uint32 picked = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 guid = fields[0].GetUInt32();
        if (Player::TeamForRace(fields[1].GetUInt8()) != team || m_bots.count(guid) ||
            sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false))
        {
            continue;
        }
        picked = guid;
    }
    while (!picked && result->NextRow());
    delete result;

    if (!picked || !sRandomPlayerbotMgr.AddPlayerBot(picked, 0))
    {
        return false;
    }

    FillBot& fillBot = m_bots[picked];
    fillBot.key = key;
    fillBot.team = team;
    fillBot.level = level;
    fillBot.state = BotState::LOGGING_IN;
    fillBot.stateTime = now;
    fillBot.loggedInByFill = true;
    fillBot.entered = false;
    return true;
}

void BattleGroundBotFill::UpdateBots(time_t now, std::map<FillKey, bool> const& activeKeys)
{
    uint32 prepares = 0;

    for (std::map<uint32, FillBot>::iterator itr = m_bots.begin(); itr != m_bots.end();)
    {
        uint32 guid = itr->first;
        FillBot& fillBot = itr->second;
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(guid);
        bool keyActive = activeKeys.count(fillBot.key) != 0;
        bool keep = true;

        switch (fillBot.state)
        {
            case BotState::LOGGING_IN:
                if (bot && bot->IsInWorld() && bot->GetPlayerbotAI())
                {
                    fillBot.state = BotState::PREPARING;
                    fillBot.stateTime = now;
                }
                else if (!keyActive || now - fillBot.stateTime > LOGIN_TIMEOUT)
                {
                    keep = false;
                }
                break;
            case BotState::PREPARING:
                if (!bot || !keyActive)
                {
                    keep = false;
                    break;
                }
                if (prepares >= MAX_PREPARES_PER_UPDATE || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsInCombat())
                {
                    break;
                }
                ++prepares;
                if (!PrepareBot(bot, fillBot))
                {
                    keep = false;
                    break;
                }
                JoinQueue(bot, fillBot.key);
                fillBot.state = BotState::QUEUED;
                fillBot.stateTime = now;
                break;
            case BotState::QUEUED:
                keep = bot && KeepQueuedBot(bot, fillBot, keyActive, now);
                // the invited flag stays set for the whole match: only a bot not inside yet accepts
                if (keep && !bot->InBattleGround() && bot->IsInvitedForBattleGroundQueueType(fillBot.key.queueTypeId))
                {
                    AcceptInvite(bot, fillBot.key);
                }
                else if (keep && bot->InBattleGround() && !fillBot.entered)
                {
                    fillBot.entered = true;
                    sLog.outString("BattleGroundBotFill: bot %s (%s) entered battleground type %u",
                                   bot->GetName(), fillBot.team == ALLIANCE ? "A" : "H", uint32(fillBot.key.bgTypeId));
                }
                break;
            case BotState::LEAVING:
                keep = bot && now - fillBot.stateTime < LEAVE_TIMEOUT && (bot->InBattleGround() || bot->IsBeingTeleported());
                break;
        }

        if (!keep && bot && bot->InBattleGround() && fillBot.state != BotState::LEAVING)
        {
            sLog.outString("BattleGroundBotFill: bot %s leaves its battleground (match over or no real player left)", bot->GetName());
            bot->LeaveBattleground();
            fillBot.state = BotState::LEAVING;
            fillBot.stateTime = now;
            keep = true;
        }

        if (keep)
        {
            ++itr;
            continue;
        }

        ReleaseBot(guid, fillBot, bot);
        m_bots.erase(itr++);
    }
}

bool BattleGroundBotFill::PrepareBot(Player* bot, FillBot const& fillBot)
{
    if (!bot->GetPlayerbotAI() || bot->InBattleGround() || bot->InBattleGroundQueueForBattleGroundQueueType(fillBot.key.queueTypeId))
    {
        return false;
    }

    if (bot->GetGroup())
    {
        bot->RemoveFromGroup();
    }

    // below the battleground's bracket: the queuing player's level (the random bot rotation
    // re-levels its bots on its own schedule anyway)
    // the bracket id is clamped at 0 below the battleground's minimum level, so a level 9 bot shares
    // the level 60 bracket of Alterac Valley: the level access has to be checked as well
    if (bot->GetBattleGroundBracketIdFromLevel(fillBot.key.bgTypeId) != fillBot.key.bracketId ||
        !bot->GetBGAccessByLevel(fillBot.key.bgTypeId))
    {
        PlayerbotFactory factory(bot, fillBot.level);
        factory.Randomize();
    }

    // revive, repair, full health / mana
    sRandomPlayerbotMgr.Refresh(bot);
    bot->RemoveAurasDueToSpell(SPELL_DESERTER);

    if (bot->GetBattleGroundBracketIdFromLevel(fillBot.key.bgTypeId) != fillBot.key.bracketId ||
        !bot->HasFreeBattleGroundQueueId() || !bot->GetBGAccessByLevel(fillBot.key.bgTypeId))
    {
        sLog.outError("BattleGroundBotFill: bot %s (level %u) can not join battleground type %u for level %u",
                      bot->GetName(), bot->getLevel(), uint32(fillBot.key.bgTypeId), fillBot.level);
        return false;
    }

    sLog.outDetail("BattleGroundBotFill: bot %s (%s, level %u) joins battleground type %u",
                   bot->GetName(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->getLevel(), uint32(fillBot.key.bgTypeId));
    return true;
}

void BattleGroundBotFill::JoinQueue(Player* bot, FillKey const& key)
{
    // the solo join of WorldSession::HandleBattlemasterJoinOpcode
    BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(key.bgTypeId);
    BattleGroundQueue& bgQueue = sBattleGroundMgr.m_BattleGroundQueues[key.queueTypeId];
    BattleGroundBracketId bracketId = bot->GetBattleGroundBracketIdFromLevel(key.bgTypeId);

    GroupQueueInfo* ginfo = bgQueue.AddGroup(bot, NULL, key.bgTypeId, bracketId, false);
    uint32 avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketId);
    uint32 queueSlot = bot->AddBattleGroundQueueId(key.queueTypeId);
    bot->SetBattleGroundEntryPoint();

    WorldPacket data;
    sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0);
    bot->GetSession()->SendPacket(&data);

    sBattleGroundMgr.ScheduleQueueUpdate(key.queueTypeId, key.bgTypeId, bracketId);
}

void BattleGroundBotFill::AcceptInvite(Player* bot, FillKey const& key)
{
    // the bot AI does not answer battleground invitations: the port packet on its own session
    BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(key.bgTypeId);
    if (!bg || bot->IsBeingTeleported())
    {
        return;
    }

    std::map<uint32, FillBot>::iterator itr = m_bots.find(bot->GetGUIDLow());
    if (itr == m_bots.end() || time(NULL) - itr->second.stateTime < ACCEPT_DELAY)
    {
        return;
    }

    WorldPacket packet(CMSG_BATTLEFIELD_PORT, 5);
    packet << uint32(bg->GetMapId()) << uint8(PORT_ENTER);
    bot->GetSession()->HandleBattleFieldPortOpcode(packet);
    itr->second.stateTime = time(NULL);
    sLog.outString("BattleGroundBotFill: bot %s accepts the invitation to battleground type %u", bot->GetName(), uint32(key.bgTypeId));
}

bool BattleGroundBotFill::KeepQueuedBot(Player* bot, FillBot const& fillBot, bool keyActive, time_t now) const
{
    BattleGroundQueueTypeId queueTypeId = fillBot.key.queueTypeId;

    // stays for the whole match while it has real players
    if (bot->InBattleGround())
    {
        BattleGround* bg = bot->GetBattleGround();
        return bg && bg->GetStatus() != STATUS_WAIT_LEAVE && (keyActive || HasRealPlayer(bg));
    }

    if (bot->IsInvitedForBattleGroundQueueType(queueTypeId))
    {
        return true;
    }

    if (bot->InBattleGroundQueueForBattleGroundQueueType(queueTypeId))
    {
        return keyActive;
    }

    // the queue join is still on its way - otherwise the match is over
    return now - fillBot.stateTime < QUEUE_JOIN_GRACE;
}

void BattleGroundBotFill::ReleaseBot(uint32 guid, FillBot const& fillBot, Player* bot)
{
    if (!bot)
    {
        return;
    }

    // still waiting in the queue (the player left it): out of the queue as a player would leave it
    if (bot->InBattleGroundQueueForBattleGroundQueueType(fillBot.key.queueTypeId) && !bot->InBattleGround())
    {
        BattleGroundQueue& bgQueue = sBattleGroundMgr.m_BattleGroundQueues[fillBot.key.queueTypeId];
        bgQueue.RemovePlayer(bot->GetObjectGuid(), true);
        bot->RemoveBattleGroundQueueId(fillBot.key.queueTypeId);
    }

    if (fillBot.loggedInByFill)
    {
        sLog.outDetail("BattleGroundBotFill: bot %s logs out", bot->GetName());
        sRandomPlayerbotMgr.LogoutPlayerBot(guid);
    }
}

bool BattleGroundBotFill::IsRealPlayer(Player* player)
{
    return !player->GetPlayerbotAI();
}

bool BattleGroundBotFill::IsFree(Player* bot)
{
    return bot->IsInWorld() && !bot->IsBeingTeleported() && !bot->GetGroup() && !bot->InBattleGround() &&
           !bot->InBattleGroundQueue() && !bot->IsInCombat() && bot->GetPlayerbotAI();
}

bool BattleGroundBotFill::IsWaitingInQueue(Player* player, BattleGroundQueueTypeId queueTypeId)
{
    return player->InBattleGroundQueueForBattleGroundQueueType(queueTypeId) &&
           !player->IsInvitedForBattleGroundQueueType(queueTypeId) && !player->InBattleGround();
}

bool BattleGroundBotFill::HasRealPlayer(BattleGround* bg)
{
    for (BattleGround::BattleGroundPlayerMap::const_iterator itr = bg->GetPlayers().begin(); itr != bg->GetPlayers().end(); ++itr)
    {
        if (Player* player = sObjectMgr.GetPlayer(itr->first, false))
        {
            if (IsRealPlayer(player))
            {
                return true;
            }
        }
    }
    return false;
}

#endif
