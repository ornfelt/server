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

#ifndef MANGOS_BATTLEGROUNDBOTFILL_H
#define MANGOS_BATTLEGROUNDBOTFILL_H

#if defined(USE_CUSTOM_CHANGES) && defined(ENABLE_PLAYERBOTS)

#include "Platform/Define.h"
#include "Policies/Singleton.h"
#include "SharedDefines.h"
#include "BattleGround.h"

#include <ctime>
#include <map>
#include <tuple>

class Player;

/**
 * Fills the battleground queue a real player joins with random playerbots, so both teams reach
 * the battleground's minimum size and the queue pops.
 *
 * The playerbot module has no battleground behaviour of its own: the bots are queued here, accept
 * their invitation here (the port packet on their own session) and, inside, fight with their
 * normal strategies ("pvp": the enemy players near them). Online random bots are used first,
 * offline random bot characters are logged in when the online ones run short; a bot below the
 * battleground's bracket is levelled to the queuing player's level. A bot leaves the match once it
 * is over or has no real player left, or the queue once the player left it.
 */
class BattleGroundBotFill
{
    public:
        BattleGroundBotFill();

        /// A player joined a battleground queue (called from the battlemaster join handler).
        void OnQueueJoin(Player* player, BattleGroundQueueTypeId queueTypeId, BattleGroundTypeId bgTypeId);
        /// World thread update.
        void Update(uint32 diff);

    private:
        enum class BotState
        {
            LOGGING_IN,     ///< an offline random bot, its login sent
            PREPARING,      ///< in world, gets its level and joins the queue
            QUEUED,         ///< in the queue, invited (accepted here) or in the match
            LEAVING         ///< leaves its match
        };

        struct FillKey
        {
            BattleGroundQueueTypeId queueTypeId;
            BattleGroundTypeId bgTypeId;
            BattleGroundBracketId bracketId;

            bool operator<(FillKey const& other) const
            {
                return std::tie(queueTypeId, bracketId) < std::tie(other.queueTypeId, other.bracketId);
            }
            bool operator==(FillKey const& other) const
            {
                return queueTypeId == other.queueTypeId && bracketId == other.bracketId;
            }
        };

        /// A real player in a queue.
        struct FillRequest
        {
            FillKey key;
            uint32 level;
            time_t joinTime;
        };

        struct FillBot
        {
            FillKey key;
            Team team;
            uint32 level;
            BotState state;
            time_t stateTime;
            bool loggedInByFill;    ///< logged out again when released
            bool entered;           ///< seen inside its battleground (logged once)
        };

        typedef std::map<FillKey, uint32> KeyLevelMap;

        void UpdateRequests(time_t now, KeyLevelMap& waitingKeys, std::map<FillKey, bool>& activeKeys);
        void FillQueue(FillKey const& key, uint32 level, time_t now);
        uint32 CountTeam(FillKey const& key, Team team) const;
        bool AddOnlineBot(FillKey const& key, Team team, uint32 level, time_t now);
        bool AddOfflineBot(FillKey const& key, Team team, uint32 level, time_t now);

        void UpdateBots(time_t now, std::map<FillKey, bool> const& activeKeys);
        bool PrepareBot(Player* bot, FillBot const& fillBot);
        void JoinQueue(Player* bot, FillKey const& key);
        void AcceptInvite(Player* bot, FillKey const& key);
        bool KeepQueuedBot(Player* bot, FillBot const& fillBot, bool keyActive, time_t now) const;
        void ReleaseBot(uint32 guid, FillBot const& fillBot, Player* bot);

        static bool IsRealPlayer(Player* player);
        static bool IsFree(Player* bot);
        static bool IsWaitingInQueue(Player* player, BattleGroundQueueTypeId queueTypeId);
        static bool HasRealPlayer(BattleGround* bg);

        std::map<std::pair<uint32, BattleGroundQueueTypeId>, FillRequest> m_requests;  ///< (player guid, queue type)
        std::map<uint32, FillBot> m_bots;                                               ///< bot guid
        uint32 m_updateTimer;
};

#define sBattleGroundBotFill MaNGOS::Singleton<BattleGroundBotFill>::Instance()

#endif

#endif
