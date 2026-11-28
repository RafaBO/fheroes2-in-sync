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
#include <utility>
#include <vector>

#include "game_mode.h"
#include "players.h"

// Online play (browser build): up to six humans (one host and up to five guests), each guest linked directly to the host.
//
// The host is the only one who runs the game rules for the day. Every day the host sends each guest a snapshot of the whole game (the same bytes a
// save file holds). All players then plan their routes and town actions at the same time. At the end of the day every guest sends its plan (the kingdom,
// towns and heroes it owns), the host merges the plans, lets all heroes walk and starts the next day.
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

    // How a wait for the other players ended.
    enum class WaitResult : uint8_t
    {
        DONE,
        // The player chose to leave the game while waiting.
        LEFT,
        // The host left the game for good (guests only).
        PEER_LEFT,
        // The window of the game was closed.
        QUIT
    };

    constexpr int maxGuests = 5;

    // True in the browser build, where the page provides the links.
    bool isAvailable();

    // True while a network game session is running (Settings game type has the network bit and a role is set).
    bool isNetworkGame();
    Role role();
    bool isHost();
    bool isGuest();

    // The colour this computer plays.
    PlayerColor localColor();

    // Host: true for the colour of a guest who is still in the game. The host's computer does not plan for these colours.
    bool isRemoteColor( const PlayerColor color );

    // Host: the colours of the guests who are still in the game.
    std::vector<PlayerColor> remoteColors();

    // The lobby (called from the new game menu): choose Host or Join. The host sees who joined, the guests wait for the host. Returns what the main
    // loop should do next.
    fheroes2::GameMode runLobby();

    // Host, after the map is chosen and before it is loaded: every player chooses a colour and, where the map allows it, a faction. Fills the colours and
    // races of the players and the seat of every guest. Returns false if the host cancelled.
    bool runSeatSelectionAsHost();

    // Called when the game screen starts: checks that the colours are known.
    void assignColors();

    // Host: a new day has begun. Stores a snapshot of the game and sends it to every guest.
    bool hostPublishDay();

    // Host: waits until the plan of every guest for the current day has arrived, then merges the plans into the game. The heroes of the guests keep the
    // routes they plotted. A guest who has left is replaced by the computer.
    WaitResult hostCollectGuestPlans();

    // Guest: remembers the heroes the player owns at the start of the day (so heroes dismissed during planning can be reported).
    void guestBeginDay();

    // Guest: sends the plan for the current day, waits for the host's snapshot of the next day and loads it. Returns DONE when a new day has been loaded.
    WaitResult guestSendPlanAndLoadNextDay();

    // Guest: loads the snapshot that the host sent (used when joining and every new day).
    bool loadSnapshotFromHost();

    // Tells the other side that this player leaves, then closes the links.
    void leaveSession( const std::string & reason );

    // The text the player should read after a wait ended with PEER_LEFT.
    const std::string & peerLeftText();

    // True while the guest swaps in the next day's snapshot (no fade out/in between days).
    bool isReloadingForNextDay();
    void setReloadingForNextDay( const bool value );

    // Host, while the heroes walk: the kingdoms of the guests are treated like computer players (no dialogs, battles are resolved automatically, the
    // plotted routes are followed). Restores the control types when it goes out of scope.
    class RemoteAsComputer final
    {
    public:
        RemoteAsComputer();
        RemoteAsComputer( const RemoteAsComputer & ) = delete;
        RemoteAsComputer & operator=( const RemoteAsComputer & ) = delete;
        ~RemoteAsComputer();

    private:
        std::vector<std::pair<Player *, int>> _players;
    };
}
