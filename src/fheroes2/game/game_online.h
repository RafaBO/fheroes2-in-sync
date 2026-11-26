/***************************************************************************
 *   fheroes2: https://github.com/ihhub/fheroes2                           *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game_mode.h"
#include "players.h"

// Online play (browser build): two humans over a direct link, one hosting the game.
//
// The host is the only one who runs the game rules for the day. Every day the host sends the other player a snapshot of the whole game (the same
// bytes a save file holds). Both players then plan their routes and town actions at the same time. At the end of the day the guest sends its
// plan (kingdom, towns and heroes it owns), the host merges it, lets all heroes walk and starts the next day.
namespace Online
{
    enum class Role : uint8_t
    {
        NONE,
        HOST,
        GUEST
    };

    enum class Link : uint8_t
    {
        IDLE,
        STARTING,
        WAITING,
        CONNECTING,
        CONNECTED,
        LOST,
        FAILED
    };

    // How a wait for the other player ended.
    enum class WaitResult : uint8_t
    {
        DONE,
        // The player chose to leave the game while waiting.
        LEFT,
        // The other player left the game for good.
        PEER_LEFT,
        // The window of the game was closed.
        QUIT
    };

    // True in the browser build, where the page provides the link.
    bool isAvailable();

    // True while a network game session is running (Settings game type has the network bit and a role is set).
    bool isNetworkGame();
    Role role();
    bool isHost();
    bool isGuest();

    PlayerColor localColor();
    PlayerColor remoteColor();
    bool isRemoteColor( const PlayerColor color );

    // The lobby (called from the new game menu): choose Host or Join and wait for the other player. Returns what the main loop should do next.
    fheroes2::GameMode runLobby();

    // Called when the game screen starts: decides which human colour is whose (the first human colour belongs to the host).
    void assignColors();

    // Host: a new day has begun. Stores a snapshot of the game and sends it to the guest.
    bool hostPublishDay();

    // Host: waits until the guest's plan for the current day has arrived, then merges it into the game. The guest's heroes keep the routes it plotted.
    WaitResult hostCollectGuestPlan();

    // Guest: remembers the heroes the player owns at the start of the day (so heroes dismissed during planning can be reported).
    void guestBeginDay();

    // Guest: sends the plan for the current day, waits for the host's snapshot of the next day and loads it. Returns DONE when a new day has been loaded.
    WaitResult guestSendPlanAndLoadNextDay();

    // Guest: loads the snapshot that the host sent (used when joining and every new day).
    bool loadSnapshotFromHost();

    // Tells the other side that this player leaves, then closes the link.
    void leaveSession( const std::string & reason );

    // The text the player should read after a wait ended with PEER_LEFT (or the other side ended the game).
    const std::string & peerLeftText();

    // True while the guest swaps in the next day's snapshot (no fade out/in between days).
    bool isReloadingForNextDay();
    void setReloadingForNextDay( const bool value );

    // The host keeps the game running while the guest is away: shows a small status text during planning when the other player's link is down.
    // Returns true if the link is currently down.
    bool isPeerLinkDown();

    // Host, while the heroes walk: the guest's kingdom is treated like a computer player (no dialogs, battles are resolved automatically, the plotted
    // routes are followed). Restores the control type when it goes out of scope.
    class RemoteAsComputer final
    {
    public:
        RemoteAsComputer();
        RemoteAsComputer( const RemoteAsComputer & ) = delete;
        RemoteAsComputer & operator=( const RemoteAsComputer & ) = delete;
        ~RemoteAsComputer();

    private:
        Player * _player{ nullptr };
        int _control{ 0 };
    };
}
