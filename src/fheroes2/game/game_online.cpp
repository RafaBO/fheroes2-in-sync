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

#include "game_online.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if defined( __EMSCRIPTEN__ )
#include <emscripten.h>
#endif

#include "castle.h"
#include "color.h"
#include "cursor.h"
#include "dialog.h"
#include "game.h"
#include "game_assets.h"
#include "game_exit.h"
#include "game_hotkeys.h"
#include "game_io.h"
#include "game_mainmenu_ui.h"
#include "heroes.h"
#include "icn.h"
#include "image.h"
#include "kingdom.h"
#include "localevent.h"
#include "logging.h"
#include "maps.h"
#include "maps_fileinfo.h"
#include "maps_tiles.h"
#include "race.h"
#include "save_format_version.h"
#include "screen.h"
#include "serialize.h"
#include "settings.h"
#include "tools.h"
#include "translations.h"
#include "ui_button.h"
#include "ui_dialog.h"
#include "ui_text.h"
#include "ui_tool.h"
#include "ui_window.h"
#include "world.h"

namespace
{
    // ----- The links (implemented by online.js in the page) -----

    enum MessageType : uint8_t
    {
        MSG_HELLO = 1,
        MSG_SNAPSHOT = 2,
        MSG_PLAN = 3,
        MSG_LEAVE = 4,
        // Host to guest: the table of colours and who sits at each (during the choice of colours)
        MSG_SEATS = 5,
        // Guest to host: "I want this colour and faction"
        MSG_CLAIM = 6
    };

    constexpr uint8_t protocolVersion = 2;

    struct Message
    {
        uint8_t type{ 0 };
        uint32_t day{ 0 };
        std::vector<uint8_t> payload;
    };

#if defined( __EMSCRIPTEN__ )
    // clang-format off
    bool linkAvailable()
    {
        return EM_ASM_INT( { return ( typeof window.fhNet !== 'undefined' ) ? 1 : 0; } ) != 0;
    }

    void linkHost()
    {
        EM_ASM( { window.fhNet.host(); } );
    }

    void linkJoin( const std::string & code )
    {
        EM_ASM( { window.fhNetTmp = ''; } );
        for ( const char c : code ) {
            EM_ASM( { window.fhNetTmp += String.fromCharCode( $0 ); }, static_cast<int>( static_cast<unsigned char>( c ) ) );
        }
        EM_ASM( { window.fhNet.join( window.fhNetTmp ); } );
    }

    void linkClose()
    {
        EM_ASM( { if ( typeof window.fhNet !== 'undefined' ) window.fhNet.close(); } );
    }

    int linkStateCode()
    {
        return EM_ASM_INT( { return ( typeof window.fhNet !== 'undefined' ) ? window.fhNet.stateCode() : 0; } );
    }

    uint32_t linkConnectCount()
    {
        return static_cast<uint32_t>( EM_ASM_INT( { return ( typeof window.fhNet !== 'undefined' ) ? window.fhNet.connects : 0; } ) );
    }

    int linkSlotCount()
    {
        return EM_ASM_INT( { return ( typeof window.fhNet !== 'undefined' ) ? window.fhNet.slotCount() : 0; } );
    }

    // 0 gone, 1 connected, 2 link lost
    int linkSlotState( const int slot )
    {
        return EM_ASM_INT( { return ( typeof window.fhNet !== 'undefined' ) ? window.fhNet.slotState( $0 ) : 0; }, slot );
    }

    void linkLockJoins()
    {
        EM_ASM( { if ( typeof window.fhNet !== 'undefined' ) window.fhNet.lockJoins(); } );
    }

    void linkDropSlot( const int slot )
    {
        EM_ASM( { if ( typeof window.fhNet !== 'undefined' ) window.fhNet.dropSlot( $0 ); }, slot );
    }

    std::string linkText( const bool wantError )
    {
        const int length = EM_ASM_INT( {
            var value = ( typeof window.fhNet !== 'undefined' ) ? ( $0 ? window.fhNet.error : window.fhNet.code ) : '';
            window.fhNetTmp = String( value );
            return window.fhNetTmp.length;
        }, wantError ? 1 : 0 );

        std::string result;
        for ( int i = 0; i < length; ++i ) {
            result.push_back( static_cast<char>( EM_ASM_INT( { return window.fhNetTmp.charCodeAt( $0 ) & 127; }, i ) ) );
        }

        return result;
    }

    // The host sends to the guest with the given number, a guest always sends to the host (the number is ignored).
    bool linkSend( const int slot, const std::vector<uint8_t> & data )
    {
        return EM_ASM_INT( {
            var bytes = HEAPU8.slice( $1, $1 + $2 );
            return window.fhNet.send( $0, bytes ) ? 1 : 0;
        }, slot, data.data(), static_cast<int>( data.size() ) ) != 0;
    }

    bool linkTake( int & slot, std::vector<uint8_t> & data )
    {
        const int size = EM_ASM_INT( { return ( typeof window.fhNet !== 'undefined' ) ? window.fhNet.nextSize() : -1; } );
        if ( size < 0 ) {
            return false;
        }

        slot = EM_ASM_INT( { return window.fhNet.nextSlot(); } );
        data.resize( static_cast<size_t>( size ) );
        EM_ASM( {
            var bytes = window.fhNet.pop();
            HEAPU8.set( bytes, $0 );
        }, data.data() );
        return true;
    }
    // clang-format on
#else
    bool linkAvailable()
    {
        return false;
    }

    void linkHost() {}
    void linkJoin( const std::string & ) {}
    void linkClose() {}

    int linkStateCode()
    {
        return 0;
    }

    uint32_t linkConnectCount()
    {
        return 0;
    }

    int linkSlotCount()
    {
        return 0;
    }

    int linkSlotState( const int )
    {
        return 0;
    }

    void linkLockJoins() {}
    void linkDropSlot( const int ) {}

    std::string linkText( const bool )
    {
        return {};
    }

    bool linkSend( const int, const std::vector<uint8_t> & )
    {
        return false;
    }

    bool linkTake( int &, std::vector<uint8_t> & )
    {
        return false;
    }
#endif

    Online::Link linkState()
    {
        switch ( linkStateCode() ) {
        case 1:
            return Online::Link::STARTING;
        case 2:
            return Online::Link::WAITING;
        case 3:
            return Online::Link::CONNECTING;
        case 4:
            return Online::Link::CONNECTED;
        case 5:
            return Online::Link::LOST;
        case 6:
            return Online::Link::FAILED;
        default:
            return Online::Link::IDLE;
        }
    }

    // ----- Session state -----

    constexpr int colorCount = 6;
    constexpr uint8_t ownerFree = 0xFF;
    constexpr uint8_t ownerHost = 0xFE;

    Online::Role sessionRole{ Online::Role::NONE };
    PlayerColor hostColor{ PlayerColor::NONE };
    // Guest: the colour this computer plays
    PlayerColor myColor{ PlayerColor::NONE };

    bool isReloading{ false };

    // Both sides: the last message that matters for the current step, sent again when the link comes back (guest only).
    std::vector<uint8_t> lastOutgoing;
    uint32_t lastSeenConnectCount{ 0 };

    // Host side: everything is kept per guest. A guest is the slot number the page gave it when it joined.
    struct GuestSlot
    {
        // Takes part in the game (chose a colour and has not left)
        bool isActive{ false };
        PlayerColor color{ PlayerColor::NONE };
        std::vector<uint8_t> plan;
        uint32_t planDay{ 0 };
        uint32_t mergedDay{ 0 };
        bool hasLeft{ false };
        std::string leftText;
    };

    std::array<GuestSlot, Online::maxGuests> guestSlots;

    std::vector<uint8_t> hostSnapshot;
    uint32_t hostSnapshotDay{ 0 };
    // The colours are final: nobody new may sit down.
    bool seatsAreFinal{ false };

    // The table of colours of the choice screen (host: the real one, guest: the last one received)
    struct SeatTable
    {
        std::array<bool, colorCount> isAvailable{};
        std::array<uint8_t, colorCount> race{};
        std::array<bool, colorCount> canChangeRace{};
        std::array<uint8_t, colorCount> owner{};
        std::array<bool, colorCount> isReady{};
        bool isFinal{ false };
        // The seat number of the receiver (guests only)
        uint8_t yourId{ ownerFree };

        SeatTable()
        {
            owner.fill( ownerFree );
        }
    };

    SeatTable seatTable;
    bool seatTableReceived{ false };
    bool seatTableChanged{ false };

    // Guest side
    std::vector<uint8_t> receivedSnapshot;
    uint32_t receivedSnapshotDay{ 0 };
    uint32_t guestLoadedDay{ 0 };
    std::vector<int> dayStartHeroIds;

    bool peerLeft{ false };
    std::string peerLeftMessage;

    PlayerColor colorFromIndex( const int index )
    {
        return Color::IndexToColor( index );
    }

    std::vector<uint8_t> encode( const uint8_t type, const uint32_t day, const std::vector<uint8_t> & payload )
    {
        std::vector<uint8_t> result;
        result.reserve( payload.size() + 5 );
        result.push_back( type );
        for ( int shift = 0; shift < 32; shift += 8 ) {
            result.push_back( static_cast<uint8_t>( ( day >> shift ) & 0xFF ) );
        }
        result.insert( result.end(), payload.begin(), payload.end() );
        return result;
    }

    bool decode( std::vector<uint8_t> && raw, Message & message )
    {
        if ( raw.size() < 5 ) {
            return false;
        }

        message.type = raw[0];
        message.day = static_cast<uint32_t>( raw[1] ) | ( static_cast<uint32_t>( raw[2] ) << 8 ) | ( static_cast<uint32_t>( raw[3] ) << 16 )
                      | ( static_cast<uint32_t>( raw[4] ) << 24 );
        message.payload.assign( raw.begin() + 5, raw.end() );
        return true;
    }

    // Host: number of guests that ever joined (the slots the page knows about)
    int knownSlots()
    {
        return std::min( linkSlotCount(), Online::maxGuests );
    }

    // Host: send to one guest
    bool sendToSlot( const int slot, const uint8_t type, const uint32_t day, const std::vector<uint8_t> & payload )
    {
        return linkSend( slot, encode( type, day, payload ) );
    }

    // Host: send to every guest who takes part
    void sendToAllActive( const uint8_t type, const uint32_t day, const std::vector<uint8_t> & payload )
    {
        for ( int slot = 0; slot < Online::maxGuests; ++slot ) {
            if ( guestSlots[slot].isActive ) {
                sendToSlot( slot, type, day, payload );
            }
        }
    }

    // Guest: send to the host. Remembers the message as the one to repeat when the link comes back.
    bool sendToHost( const uint8_t type, const uint32_t day, const std::vector<uint8_t> & payload, const bool rememberForResend )
    {
        std::vector<uint8_t> data = encode( type, day, payload );
        if ( rememberForResend ) {
            lastOutgoing = data;
        }

        return linkSend( 0, data );
    }

    std::string snapshotPath()
    {
        return ( std::filesystem::temp_directory_path() / "fh2_online_snapshot.sav" ).string();
    }

    bool readFileBytes( const std::string & path, std::vector<uint8_t> & bytes )
    {
        std::ifstream file( path, std::ios::binary );
        if ( !file ) {
            return false;
        }

        bytes.assign( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
        return !bytes.empty();
    }

    bool writeFileBytes( const std::string & path, const std::vector<uint8_t> & bytes )
    {
        std::ofstream file( path, std::ios::binary | std::ios::trunc );
        if ( !file ) {
            return false;
        }

        file.write( reinterpret_cast<const char *>( bytes.data() ), static_cast<std::streamsize>( bytes.size() ) );
        return static_cast<bool>( file );
    }

    // ----- The plan a guest sends (the parts of the game that belong to the guest) -----

    void putChunk( OStreamBase & out, const RWStreamBuf & chunk )
    {
        out.put32( static_cast<uint32_t>( chunk.size() ) );
        out.putRaw( chunk.data(), chunk.size() );
    }

    bool getChunk( IStreamBase & in, std::vector<uint8_t> & bytes )
    {
        const uint32_t size = in.get32();
        if ( in.fail() || size > 16u * 1024u * 1024u ) {
            return false;
        }

        bytes = in.getRaw( size );
        return !in.fail();
    }

    std::vector<uint8_t> buildGuestPlan()
    {
        Game::SetVersionOfCurrentSaveFile( CURRENT_FORMAT_VERSION );

        const PlayerColor color = myColor;
        Kingdom & kingdom = world.GetKingdom( color );

        RWStreamBuf out;
        out.setBigendian( true );

        out.put32( protocolVersion );

        {
            RWStreamBuf chunk;
            chunk.setBigendian( true );
            chunk << kingdom;
            putChunk( out, chunk );
        }

        const VecCastles & castles = kingdom.GetCastles();
        out.put32( static_cast<uint32_t>( castles.size() ) );
        for ( const Castle * castle : castles ) {
            out.put32( static_cast<uint32_t>( castle->GetIndex() ) );

            RWStreamBuf chunk;
            chunk.setBigendian( true );
            chunk << *castle;
            putChunk( out, chunk );
        }

        // The heroes the player owns now plus the ones owned at the start of the day (a dismissed hero still has to be reported).
        std::set<int> heroIds( dayStartHeroIds.begin(), dayStartHeroIds.end() );
        for ( const Heroes * hero : kingdom.GetHeroes() ) {
            heroIds.insert( hero->GetID() );
        }

        out.put32( static_cast<uint32_t>( heroIds.size() ) );
        for ( const int id : heroIds ) {
            out.put32( static_cast<uint32_t>( id ) );

            const Heroes * hero = world.GetHeroes( id );
            assert( hero != nullptr );

            RWStreamBuf chunk;
            chunk.setBigendian( true );
            chunk << *hero;
            hero->writeSimultaneousPlan( chunk );
            putChunk( out, chunk );
        }

        return std::vector<uint8_t>( out.data(), out.data() + out.size() );
    }

    // Merges the plan of a guest into the host's game. Returns false if the plan could not be read.
    bool applyGuestPlan( const std::vector<uint8_t> & bytes, const PlayerColor color )
    {
        Game::SetVersionOfCurrentSaveFile( CURRENT_FORMAT_VERSION );

        Kingdom & kingdom = world.GetKingdom( color );

        ROStreamBuf in( bytes );
        in.setBigendian( true );

        if ( in.get32() != protocolVersion ) {
            return false;
        }

        std::vector<uint8_t> chunk;

        if ( !getChunk( in, chunk ) ) {
            return false;
        }

        {
            ROStreamBuf sub( std::move( chunk ) );
            sub.setBigendian( true );
            sub >> kingdom;
            if ( sub.fail() ) {
                return false;
            }
        }

        const uint32_t castleCount = in.get32();
        for ( uint32_t i = 0; i < castleCount && !in.fail(); ++i ) {
            const int32_t index = static_cast<int32_t>( in.get32() );
            if ( !getChunk( in, chunk ) ) {
                return false;
            }

            Castle * castle = world.getCastleEntrance( Maps::GetPoint( index ) );
            if ( castle == nullptr || castle->GetColor() != color ) {
                continue;
            }

            ROStreamBuf sub( std::move( chunk ) );
            sub.setBigendian( true );
            sub >> *castle;
        }

        const uint32_t heroCount = in.get32();
        for ( uint32_t i = 0; i < heroCount && !in.fail(); ++i ) {
            const int id = static_cast<int>( in.get32() );
            if ( !getChunk( in, chunk ) ) {
                return false;
            }

            Heroes * hero = Heroes::isValidId( id ) ? world.GetHeroes( id ) : nullptr;
            if ( hero == nullptr ) {
                continue;
            }

            // A hero that the host's game gave to somebody else in the meantime cannot be taken (two players hired the same hero on the same day).
            if ( hero->GetColor() != PlayerColor::NONE && hero->GetColor() != color ) {
                DEBUG_LOG( DBG_GAME, DBG_WARN, "online: hero " << id << " belongs to another player, the guest's copy is ignored" )

                auto & guestHeroes = kingdom.GetHeroes();
                guestHeroes.erase( std::remove( guestHeroes.begin(), guestHeroes.end(), hero ), guestHeroes.end() );
                continue;
            }

            const int32_t oldIndex = hero->GetIndex();
            const MP2::MapObjectType oldObjectUnderHero = hero->getObjectTypeUnderHero();

            ROStreamBuf sub( std::move( chunk ) );
            sub.setBigendian( true );
            sub >> *hero;
            hero->readSimultaneousPlan( sub );

            const int32_t newIndex = hero->GetIndex();
            if ( oldIndex != newIndex ) {
                // Hired, dismissed or moved by a spell during planning: the map has to follow.
                if ( oldIndex >= 0 && world.getTile( oldIndex ).getMainObjectType() == MP2::OBJ_HERO ) {
                    // Reading the hero replaced what he stands on; the old tile needs the old value to be put right.
                    hero->setObjectTypeUnderHero( oldObjectUnderHero );
                    world.getTile( oldIndex ).setHero( nullptr );
                }

                if ( newIndex >= 0 ) {
                    world.getTile( newIndex ).setHero( hero );
                    hero->Scout( newIndex );
                }
            }
        }

        return !in.fail();
    }

    // ----- The table of colours -----

    std::vector<uint8_t> encodeSeatTable( const SeatTable & table, const uint8_t yourId )
    {
        std::vector<uint8_t> out;
        out.push_back( table.isFinal ? 1 : 0 );
        out.push_back( yourId );

        for ( int i = 0; i < colorCount; ++i ) {
            out.push_back( table.isAvailable[i] ? 1 : 0 );
            out.push_back( table.race[i] );
            out.push_back( table.canChangeRace[i] ? 1 : 0 );
            out.push_back( table.owner[i] );
            out.push_back( table.isReady[i] ? 1 : 0 );
        }

        return out;
    }

    bool decodeSeatTable( const std::vector<uint8_t> & in, SeatTable & table )
    {
        if ( in.size() != static_cast<size_t>( 2 + colorCount * 5 ) ) {
            return false;
        }

        table.isFinal = in[0] != 0;
        table.yourId = in[1];

        size_t pos = 2;
        for ( int i = 0; i < colorCount; ++i ) {
            table.isAvailable[i] = in[pos++] != 0;
            table.race[i] = in[pos++];
            table.canChangeRace[i] = in[pos++] != 0;
            table.owner[i] = in[pos++];
            table.isReady[i] = in[pos++] != 0;
        }

        return true;
    }

    // Host: a player (the host or a guest) takes a colour, or changes the faction of the colour he already has.
    void claimSeat( SeatTable & table, const uint8_t owner, const int color, const uint8_t race, const bool isReady )
    {
        if ( color < 0 || color >= colorCount || !table.isAvailable[color] ) {
            return;
        }

        if ( table.owner[color] != ownerFree && table.owner[color] != owner ) {
            return;
        }

        const bool isSameSeat = table.owner[color] == owner;
        const uint8_t oldRace = table.race[color];

        for ( int i = 0; i < colorCount; ++i ) {
            if ( i != color && table.owner[i] == owner ) {
                table.owner[i] = ownerFree;
                table.isReady[i] = false;
            }
        }

        table.owner[color] = owner;

        if ( table.canChangeRace[color] ) {
            table.race[color] = race;
        }

        // Changing the colour or the faction takes the "ready" away.
        table.isReady[color] = isReady && isSameSeat && oldRace == table.race[color];
        seatTableChanged = true;
    }

    int seatOfOwner( const SeatTable & table, const uint8_t owner )
    {
        for ( int i = 0; i < colorCount; ++i ) {
            if ( table.owner[i] == owner ) {
                return i;
            }
        }

        return -1;
    }

    void releaseSeatsOfOwner( SeatTable & table, const uint8_t owner )
    {
        for ( int i = 0; i < colorCount; ++i ) {
            if ( table.owner[i] == owner ) {
                table.owner[i] = ownerFree;
                table.isReady[i] = false;
                seatTableChanged = true;
            }
        }
    }

    std::string ownerName( const uint8_t owner )
    {
        if ( owner == ownerFree ) {
            return _( "free" );
        }

        if ( owner == ownerHost ) {
            return _( "host" );
        }

        std::string text = _( "guest %{number}" );
        StringReplace( text, "%{number}", std::to_string( static_cast<int>( owner ) + 1 ) );
        return text;
    }

    // ----- Receiving -----

    // Host: a guest leaves for good. Its colour becomes a computer player.
    void guestLeaves( const int slot, const std::string & text )
    {
        GuestSlot & guest = guestSlots[slot];
        if ( !guest.isActive && !guest.hasLeft ) {
            // He had not taken a seat yet.
            seatTableChanged = true;
        }

        if ( guest.isActive ) {
            guest.isActive = false;
            guest.hasLeft = true;
            guest.leftText = text;
        }

        releaseSeatsOfOwner( seatTable, static_cast<uint8_t>( slot ) );
    }

    void handleMessageAsHost( const int slot, const Message & message )
    {
        if ( slot < 0 || slot >= Online::maxGuests ) {
            return;
        }

        GuestSlot & guest = guestSlots[slot];

        switch ( message.type ) {
        case MSG_LEAVE:
            guestLeaves( slot, std::string( message.payload.begin(), message.payload.end() ) );
            break;
        case MSG_HELLO:
            if ( seatsAreFinal && !guest.isActive ) {
                // A stranger, or somebody who was replaced by the computer.
                linkDropSlot( slot );
            }
            else if ( guest.isActive && !hostSnapshot.empty() && message.day != hostSnapshotDay ) {
                // The guest is behind (it came back after losing its game): give it today's snapshot.
                std::vector<uint8_t> payload{ static_cast<uint8_t>( guest.color ), static_cast<uint8_t>( hostColor ) };
                payload.insert( payload.end(), hostSnapshot.begin(), hostSnapshot.end() );
                sendToSlot( slot, MSG_SNAPSHOT, hostSnapshotDay, payload );
            }
            else if ( !seatsAreFinal ) {
                // Somebody who joins while the colours are chosen gets the table.
                seatTableChanged = true;
            }
            break;
        case MSG_PLAN:
            if ( guest.isActive && message.day > guest.planDay ) {
                guest.plan = message.payload;
                guest.planDay = message.day;
            }
            break;
        case MSG_CLAIM:
            if ( !seatsAreFinal && message.payload.size() == 3 ) {
                const int colorIndex = message.payload[0];
                claimSeat( seatTable, static_cast<uint8_t>( slot ), colorIndex, message.payload[1], message.payload[2] != 0 );
            }
            break;
        default:
            break;
        }
    }

    void handleMessageAsGuest( const Message & message )
    {
        switch ( message.type ) {
        case MSG_LEAVE:
            peerLeft = true;
            peerLeftMessage.assign( message.payload.begin(), message.payload.end() );
            break;
        case MSG_SNAPSHOT:
            if ( message.day > receivedSnapshotDay && message.payload.size() > 2 ) {
                myColor = static_cast<PlayerColor>( message.payload[0] );
                hostColor = static_cast<PlayerColor>( message.payload[1] );
                receivedSnapshot.assign( message.payload.begin() + 2, message.payload.end() );
                receivedSnapshotDay = message.day;
            }
            break;
        case MSG_SEATS: {
            SeatTable table;
            if ( decodeSeatTable( message.payload, table ) ) {
                seatTable = table;
                seatTableReceived = true;
                seatTableChanged = true;
            }
            break;
        }
        default:
            break;
        }
    }

    // Reads everything that has arrived and, for a guest, repeats the last message after the link came back.
    void pumpMessages()
    {
        if ( sessionRole == Online::Role::GUEST ) {
            const uint32_t connectCount = linkConnectCount();
            if ( connectCount != lastSeenConnectCount ) {
                lastSeenConnectCount = connectCount;

                if ( connectCount > 0 ) {
                    linkSend( 0, encode( MSG_HELLO, guestLoadedDay, { protocolVersion } ) );

                    if ( !lastOutgoing.empty() ) {
                        linkSend( 0, lastOutgoing );
                    }
                }
            }
        }

        int slot = 0;
        std::vector<uint8_t> raw;
        while ( linkTake( slot, raw ) ) {
            Message message;
            if ( decode( std::move( raw ), message ) ) {
                if ( sessionRole == Online::Role::HOST ) {
                    handleMessageAsHost( slot, message );
                }
                else if ( sessionRole == Online::Role::GUEST ) {
                    handleMessageAsGuest( message );
                }
            }
            raw.clear();
        }
    }

    // ----- Dialogs -----

    class DialogWindow
    {
    public:
        DialogWindow( const int32_t width, const int32_t height, const std::string & title, const char * buttonText, const char * secondButtonText = nullptr )
            : _display( fheroes2::Display::instance() )
            , _restorer( _display, ( _display.width() - width ) / 2 - 16, ( _display.height() - height ) / 2 - 16, width + 32, height + 32 )
            , _window( width, height, true, _display )
        {
            const fheroes2::Rect & area = _window.activeArea();

            fheroes2::Text titleText( title, fheroes2::FontType::normalYellow() );
            titleText.draw( area.x + 10, area.y + 12, area.width - 20, _display );

            _bodyArea = { area.x + 15, area.y + 40, area.width - 30, area.height - 40 - 50 };
            _bodyRestorer = std::make_unique<fheroes2::ImageRestorer>( _display, _bodyArea.x, _bodyArea.y, _bodyArea.width, _bodyArea.height );

            if ( secondButtonText != nullptr ) {
                _window.renderTextAdaptedButtonSprite( _button, buttonText, { 20, 10 }, fheroes2::StandardWindow::Padding::BOTTOM_LEFT );
                _window.renderTextAdaptedButtonSprite( _secondButton, secondButtonText, { 20, 10 }, fheroes2::StandardWindow::Padding::BOTTOM_RIGHT );
                _hasSecondButton = true;
            }
            else {
                _window.renderTextAdaptedButtonSprite( _button, buttonText, { 0, 10 }, fheroes2::StandardWindow::Padding::BOTTOM_CENTER );
            }

            _display.render( _window.totalArea() );
        }

        void setBody( const std::string & text, const bool large = false )
        {
            if ( text == _lastBody ) {
                return;
            }

            _lastBody = text;
            _bodyRestorer->restore();

            fheroes2::Text body( text, large ? fheroes2::FontType::largeWhite() : fheroes2::FontType::normalWhite() );
            const int32_t height = body.height( _bodyArea.width );
            body.draw( _bodyArea.x, _bodyArea.y + std::max( 0, ( _bodyArea.height - height ) / 2 ), _bodyArea.width, _display );

            _display.render( _bodyArea );
        }

        fheroes2::ButtonSprite & button()
        {
            return _button;
        }

        fheroes2::ButtonSprite & secondButton()
        {
            return _secondButton;
        }

        bool hasSecondButton() const
        {
            return _hasSecondButton;
        }

        const fheroes2::Rect & activeArea() const
        {
            return _window.activeArea();
        }

        void renderAll()
        {
            _display.render( _window.totalArea() );
        }

    private:
        fheroes2::Display & _display;
        fheroes2::ImageRestorer _restorer;
        fheroes2::StandardWindow _window;
        fheroes2::Rect _bodyArea;
        std::unique_ptr<fheroes2::ImageRestorer> _bodyRestorer;
        fheroes2::ButtonSprite _button;
        fheroes2::ButtonSprite _secondButton;
        bool _hasSecondButton{ false };
        std::string _lastBody;
    };

    // Host: the guests who take part and have not got a plan for the day yet
    std::vector<int> slotsWithoutPlan( const uint32_t day )
    {
        std::vector<int> result;
        for ( int slot = 0; slot < Online::maxGuests; ++slot ) {
            if ( guestSlots[slot].isActive && guestSlots[slot].planDay < day ) {
                result.push_back( slot );
            }
        }

        return result;
    }

    // Host: the guests who take part and whose link is down
    std::vector<int> slotsWithLinkDown()
    {
        std::vector<int> result;
        for ( int slot = 0; slot < Online::maxGuests; ++slot ) {
            if ( guestSlots[slot].isActive && linkSlotState( slot ) != 1 ) {
                result.push_back( slot );
            }
        }

        return result;
    }

    std::string colorName( const PlayerColor color )
    {
        return Color::String( color );
    }

    // Host: this guest does not come back. His colour is played by the computer from now on.
    void replaceGuestByComputer( const int slot, const std::string & text )
    {
        GuestSlot & guest = guestSlots[slot];
        const PlayerColor color = guest.color;

        if ( guest.isActive ) {
            guest.isActive = false;
            guest.hasLeft = true;
            guest.leftText = text;
        }

        linkDropSlot( slot );

        if ( Player * player = Players::Get( color ); player != nullptr ) {
            player->SetControl( CONTROL_AI );
        }
    }

    // Shows a small window until the condition is true. While a link is down the text says the game is paused. Returns how the wait ended.
    Online::WaitResult waitForEvent( const std::function<bool()> & isDone, const std::function<std::string()> & makeText, const bool hostCanDropPlayers )
    {
        pumpMessages();

        if ( isDone() ) {
            return Online::WaitResult::DONE;
        }

        if ( peerLeft ) {
            return Online::WaitResult::PEER_LEFT;
        }

        const CursorRestorer cursorRestorer( true, Cursor::POINTER );

        DialogWindow dialog( 440, 200, _( "Online game" ), _( "LEAVE" ), hostCanDropPlayers ? _( "DROP MISSING" ) : nullptr );
        dialog.setBody( makeText() );

        LocalEvent & le = LocalEvent::Get();

        while ( true ) {
            if ( !le.HandleEvents( true, true ) ) {
                if ( Game::processExitEvent() == fheroes2::GameMode::QUIT_GAME ) {
                    return Online::WaitResult::QUIT;
                }

                continue;
            }

            pumpMessages();

            if ( isDone() ) {
                return Online::WaitResult::DONE;
            }

            if ( peerLeft ) {
                return Online::WaitResult::PEER_LEFT;
            }

            dialog.setBody( makeText() );

            dialog.button().drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( dialog.button().area() ) );
            if ( dialog.hasSecondButton() ) {
                dialog.secondButton().drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( dialog.secondButton().area() ) );
            }

            dialog.renderAll();

            if ( le.MouseClickLeft( dialog.button().area() ) ) {
                if ( fheroes2::showStandardTextMessage( _( "Leave game" ), _( "Do you really want to leave the online game? The other players will be told." ),
                                                       Dialog::YES | Dialog::NO )
                     == Dialog::YES ) {
                    return Online::WaitResult::LEFT;
                }
            }
            else if ( dialog.hasSecondButton() && le.MouseClickLeft( dialog.secondButton().area() ) ) {
                const std::vector<int> missing = slotsWithLinkDown();
                if ( !missing.empty()
                     && fheroes2::showStandardTextMessage( _( "Drop missing players" ),
                                                           _( "The players whose connection is lost are taken out of the game and the computer plays their colour from now on. Do this?" ),
                                                           Dialog::YES | Dialog::NO )
                            == Dialog::YES ) {
                    for ( const int slot : missing ) {
                        replaceGuestByComputer( slot, "connection lost" );
                    }
                }
            }
        }
    }

    // ----- The colour and faction choice screen -----

    // Draws the table and handles the clicks. isHost: the table is the real one and the host arbitrates. Returns when the host starts the game (host: all are
    // ready) or the host has finished the choice (guest: the final table arrived), or on leave/quit.
    enum class SeatResult : uint8_t
    {
        FINISHED,
        LEFT,
        QUIT,
        HOST_GONE
    };

    SeatResult runSeatDialog( const bool isHost )
    {
        fheroes2::Display & display = fheroes2::Display::instance();
        LocalEvent & le = LocalEvent::Get();
        const CursorRestorer cursorRestorer( true, Cursor::POINTER );

        const int32_t width = 540;
        const int32_t height = 330;

        DialogWindow dialog( width, height, _( "Choose your colour and faction" ), _( "READY" ), _( "LEAVE" ) );
        const fheroes2::Rect area = dialog.activeArea();

        const fheroes2::Rect tableArea( area.x + 10, area.y + 38, area.width - 20, area.height - 38 - 56 );
        fheroes2::ImageRestorer tableRestorer( display, tableArea.x, tableArea.y, tableArea.width, tableArea.height );

        const int32_t columnWidth = tableArea.width / colorCount;

        std::array<fheroes2::Rect, colorCount> colorRects;
        std::array<fheroes2::Rect, colorCount> raceRects;

        const auto myId = [isHost]() { return isHost ? ownerHost : seatTable.yourId; };

        const auto redraw = [&]() {
            tableRestorer.restore();

            for ( int i = 0; i < colorCount; ++i ) {
                const int32_t x = tableArea.x + i * columnWidth;
                const fheroes2::Sprite & colorIcon = Assets::getImage( ICN::NGEXTRA, static_cast<uint32_t>( i ) + 3 );
                const bool isAvailable = seatTable.isAvailable[i];
                const uint8_t owner = seatTable.owner[i];

                colorRects[i] = { x + ( columnWidth - colorIcon.width() ) / 2, tableArea.y + 6, colorIcon.width(), colorIcon.height() };
                raceRects[i] = {};

                if ( !isAvailable ) {
                    // A colour no human may take: just a dim label
                    const fheroes2::Text text( _( "computer" ), fheroes2::FontType::smallWhite() );
                    text.draw( x, tableArea.y + 40, columnWidth, display );
                    continue;
                }

                fheroes2::Copy( colorIcon, 0, 0, display, colorRects[i].x, colorRects[i].y, colorRects[i].width, colorRects[i].height );

                const fheroes2::Sprite & raceIcon = Assets::getImage( ICN::NGEXTRA, Race::getRaceIcnIndex( seatTable.race[i], owner != ownerFree ) );
                raceRects[i] = { x + ( columnWidth - raceIcon.width() ) / 2, colorRects[i].y + colorRects[i].height + 14, raceIcon.width(), raceIcon.height() };
                fheroes2::Copy( raceIcon, 0, 0, display, raceRects[i].x, raceRects[i].y, raceRects[i].width, raceRects[i].height );

                std::string label = owner == myId() ? std::string( _( "YOU" ) ) : ownerName( owner );
                if ( owner != ownerFree && seatTable.isReady[i] ) {
                    label += "\n" + std::string( _( "ready" ) );
                }
                else if ( owner != ownerFree && !seatTable.canChangeRace[i] ) {
                    label += "\n" + std::string( _( "fixed faction" ) );
                }

                const fheroes2::Text text( label, owner == myId() ? fheroes2::FontType::normalYellow() : fheroes2::FontType::smallWhite() );
                text.draw( x, raceRects[i].y + raceRects[i].height + 8, columnWidth, display );
            }

            display.render( tableArea );
        };

        // The host first publishes the table, a guest waits for it.
        const auto publishTable = [&]() {
            for ( int slot = 0; slot < Online::maxGuests; ++slot ) {
                if ( guestSlots[slot].isActive ) {
                    sendToSlot( slot, MSG_SEATS, 0, encodeSeatTable( seatTable, static_cast<uint8_t>( slot ) ) );
                }
            }
        };

        if ( isHost ) {
            publishTable();
        }

        seatTableChanged = true;
        std::string lastStatus;

        // The clicks of this player
        const auto sendMyChoice = [&]( const int color, const uint8_t race, const bool isReady ) {
            if ( isHost ) {
                claimSeat( seatTable, ownerHost, color, race, isReady );
            }
            else {
                sendToHost( MSG_CLAIM, 0, { static_cast<uint8_t>( color ), race, static_cast<uint8_t>( isReady ? 1 : 0 ) }, false );
            }
        };

        while ( true ) {
            if ( !le.HandleEvents( true, true ) ) {
                if ( Game::processExitEvent() == fheroes2::GameMode::QUIT_GAME ) {
                    return SeatResult::QUIT;
                }

                continue;
            }

            pumpMessages();

            if ( peerLeft ) {
                return SeatResult::HOST_GONE;
            }

            if ( !isHost && ( seatTable.isFinal || receivedSnapshotDay > 0 ) ) {
                return SeatResult::FINISHED;
            }

            if ( isHost && seatTableChanged ) {
                // Players who left free their colour; everyone gets the new table.
                publishTable();
            }

            const bool hasTable = isHost || seatTableReceived;

            if ( seatTableChanged || lastStatus.empty() ) {
                seatTableChanged = false;
                lastStatus = "x";

                if ( hasTable ) {
                    redraw();
                }
            }

            dialog.button().drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( dialog.button().area() ) );
            dialog.secondButton().drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( dialog.secondButton().area() ) );
            dialog.renderAll();

            if ( !hasTable ) {
                continue;
            }

            const int mySeat = seatOfOwner( seatTable, myId() );

            for ( int i = 0; i < colorCount; ++i ) {
                if ( !seatTable.isAvailable[i] ) {
                    continue;
                }

                if ( le.MouseClickLeft( colorRects[i] ) || le.MouseClickLeft( raceRects[i] ) ) {
                    if ( i == mySeat ) {
                        // Own colour: a click on the faction changes it (where the map allows it).
                        if ( le.MouseClickLeft( raceRects[i] ) && seatTable.canChangeRace[i] ) {
                            sendMyChoice( i, static_cast<uint8_t>( Race::getNextRace( seatTable.race[i] ) ), false );
                        }
                    }
                    else if ( seatTable.owner[i] == ownerFree ) {
                        // A free colour: move there, keeping the faction that was chosen if the map lets us choose.
                        const uint8_t race = mySeat >= 0 && seatTable.canChangeRace[i] ? seatTable.race[mySeat] : seatTable.race[i];
                        sendMyChoice( i, race, false );
                    }
                }
                else if ( i == mySeat && seatTable.canChangeRace[i] && le.isMouseRightButtonPressedInArea( raceRects[i] ) ) {
                    sendMyChoice( i, static_cast<uint8_t>( Race::getPreviousRace( seatTable.race[i] ) ), false );
                }
            }

            if ( mySeat >= 0 && le.MouseClickLeft( dialog.button().area() ) ) {
                sendMyChoice( mySeat, seatTable.race[mySeat], !seatTable.isReady[mySeat] );
            }

            if ( le.MouseClickLeft( dialog.secondButton().area() ) ) {
                if ( fheroes2::showStandardTextMessage( _( "Leave game" ), _( "Do you really want to leave the online game? The other players will be told." ),
                                                       Dialog::YES | Dialog::NO )
                     == Dialog::YES ) {
                    return SeatResult::LEFT;
                }
            }

            if ( isHost ) {
                // Start as soon as everybody who sits at a colour is ready.
                bool isEverybodyReady = true;
                int seated = 0;
                for ( int i = 0; i < colorCount; ++i ) {
                    if ( seatTable.owner[i] != ownerFree ) {
                        ++seated;
                        isEverybodyReady = isEverybodyReady && seatTable.isReady[i];
                    }
                }

                if ( seated > 0 && isEverybodyReady ) {
                    return SeatResult::FINISHED;
                }
            }
        }
    }

    // The colours of the human players that the map offers, with the faction the map or the setup screen gave them.
    void fillSeatTableFromMap()
    {
        const Settings & conf = Settings::Get();
        const Maps::FileInfo & info = conf.getCurrentMapInfo();
        const PlayerColorsSet humanColors = info.colorsAvailableForHumans;

        seatTable = SeatTable();

        for ( int i = 0; i < colorCount; ++i ) {
            const PlayerColor color = colorFromIndex( i );
            const Player * player = Players::Get( color );
            const bool isAvailable = player != nullptr && ( humanColors & color ) != 0;

            seatTable.isAvailable[i] = isAvailable;
            seatTable.race[i] = player != nullptr ? static_cast<uint8_t>( player->GetRace() ) : static_cast<uint8_t>( Race::RAND );
            seatTable.canChangeRace[i] = isAvailable && info.AllowChangeRace( color );
        }
    }

    // Gives every player a first colour: the colours the setup screen marked as human come first.
    void assignFirstSeats( const std::vector<int> & activeSlots )
    {
        std::vector<int> order;
        for ( int i = 0; i < colorCount; ++i ) {
            const Player * player = Players::Get( colorFromIndex( i ) );
            if ( seatTable.isAvailable[i] && player != nullptr && player->isControlHuman() ) {
                order.push_back( i );
            }
        }

        for ( int i = 0; i < colorCount; ++i ) {
            if ( seatTable.isAvailable[i] && std::find( order.begin(), order.end(), i ) == order.end() ) {
                order.push_back( i );
            }
        }

        size_t next = 0;
        if ( next < order.size() ) {
            seatTable.owner[order[next++]] = ownerHost;
        }

        for ( const int slot : activeSlots ) {
            if ( next < order.size() ) {
                seatTable.owner[order[next++]] = static_cast<uint8_t>( slot );
            }
        }
    }
}

namespace Online
{
    bool isAvailable()
    {
        return linkAvailable();
    }

    bool isNetworkGame()
    {
        return sessionRole != Role::NONE && Settings::Get().IsGameType( Game::TYPE_NETWORK );
    }

    Role role()
    {
        return sessionRole;
    }

    bool isHost()
    {
        return sessionRole == Role::HOST;
    }

    bool isGuest()
    {
        return sessionRole == Role::GUEST;
    }

    PlayerColor localColor()
    {
        return sessionRole == Role::GUEST ? myColor : hostColor;
    }

    std::vector<PlayerColor> remoteColors()
    {
        std::vector<PlayerColor> result;

        if ( sessionRole == Role::HOST ) {
            for ( const GuestSlot & guest : guestSlots ) {
                if ( guest.isActive && guest.color != PlayerColor::NONE ) {
                    result.push_back( guest.color );
                }
            }
        }

        return result;
    }

    bool isRemoteColor( const PlayerColor color )
    {
        if ( !isNetworkGame() || color == PlayerColor::NONE || sessionRole != Role::HOST ) {
            return false;
        }

        return std::any_of( guestSlots.begin(), guestSlots.end(), [color]( const GuestSlot & guest ) { return guest.isActive && guest.color == color; } );
    }

    const std::string & peerLeftText()
    {
        return peerLeftMessage;
    }

    bool isReloadingForNextDay()
    {
        return isReloading;
    }

    void setReloadingForNextDay( const bool value )
    {
        isReloading = value;
    }

    void assignColors()
    {
        if ( sessionRole == Role::HOST && hostColor == PlayerColor::NONE ) {
            // The colours were not chosen (should not happen): the host takes the first human colour.
            for ( const Player * player : Settings::Get().GetPlayers() ) {
                if ( player != nullptr && player->isControlHuman() ) {
                    hostColor = player->GetColor();
                    break;
                }
            }
        }

        DEBUG_LOG( DBG_GAME, DBG_INFO, "online: this computer plays " << Color::String( localColor() ) )
    }

    bool hostPublishDay()
    {
        assert( sessionRole == Role::HOST );

        const std::string path = snapshotPath();

        if ( !Game::Save( path, true ) || !readFileBytes( path, hostSnapshot ) ) {
            DEBUG_LOG( DBG_GAME, DBG_WARN, "online: could not make the snapshot of the day" )
            return false;
        }

        hostSnapshotDay = world.CountDay();

        std::remove( path.c_str() );

        // A guest whose link is down gets the snapshot when it comes back and says hello.
        for ( int slot = 0; slot < maxGuests; ++slot ) {
            if ( !guestSlots[slot].isActive ) {
                continue;
            }

            std::vector<uint8_t> payload{ static_cast<uint8_t>( guestSlots[slot].color ), static_cast<uint8_t>( hostColor ) };
            payload.insert( payload.end(), hostSnapshot.begin(), hostSnapshot.end() );
            sendToSlot( slot, MSG_SNAPSHOT, hostSnapshotDay, payload );
        }

        return true;
    }

    WaitResult hostCollectGuestPlans()
    {
        assert( sessionRole == Role::HOST );

        const uint32_t day = world.CountDay();

        const WaitResult result = waitForEvent(
            [day]() { return slotsWithoutPlan( day ).empty(); },
            [day]() {
                const std::vector<int> down = slotsWithLinkDown();
                if ( !down.empty() ) {
                    std::string names;
                    for ( const int slot : down ) {
                        names += ( names.empty() ? "" : ", " ) + colorName( guestSlots[slot].color );
                    }

                    std::string text = _( "The connection to %{players} was lost. The game is paused.\n\nIt goes on as soon as they are back. Room code: %{code}" );
                    StringReplace( text, "%{players}", names );
                    StringReplace( text, "%{code}", linkText( false ) );
                    return text;
                }

                std::string text = _( "Waiting for %{count} player(s) to finish planning the day..." );
                StringReplace( text, "%{count}", std::to_string( slotsWithoutPlan( day ).size() ) );
                return text;
            },
            true );

        if ( result != WaitResult::DONE ) {
            return result;
        }

        // Guests who left or were dropped: the computer plays their colour.
        for ( int slot = 0; slot < maxGuests; ++slot ) {
            GuestSlot & guest = guestSlots[slot];

            if ( guest.hasLeft && guest.color != PlayerColor::NONE ) {
                if ( Player * player = Players::Get( guest.color ); player != nullptr && player->isControlHuman() ) {
                    player->SetControl( CONTROL_AI );

                    std::string text = _( "The %{color} player has left the game. The computer plays this colour from now on." );
                    StringReplace( text, "%{color}", colorName( guest.color ) );
                    fheroes2::showStandardTextMessage( _( "Online game" ), text, Dialog::OK );
                }
            }
        }

        for ( int slot = 0; slot < maxGuests; ++slot ) {
            GuestSlot & guest = guestSlots[slot];

            if ( guest.isActive && guest.mergedDay < day ) {
                if ( !applyGuestPlan( guest.plan, guest.color ) ) {
                    DEBUG_LOG( DBG_GAME, DBG_WARN, "online: the plan of guest " << slot << " could not be read" )
                }

                guest.mergedDay = day;
            }
        }

        return WaitResult::DONE;
    }

    void guestBeginDay()
    {
        dayStartHeroIds.clear();

        for ( const Heroes * hero : world.GetKingdom( myColor ).GetHeroes() ) {
            dayStartHeroIds.push_back( hero->GetID() );
        }

        lastOutgoing.clear();
    }

    WaitResult guestSendPlanAndLoadNextDay()
    {
        assert( sessionRole == Role::GUEST );

        const uint32_t day = world.CountDay();

        sendToHost( MSG_PLAN, day, buildGuestPlan(), true );
        lastSeenConnectCount = linkConnectCount();

        const WaitResult result = waitForEvent(
            [day]() { return receivedSnapshotDay > day; },
            []() {
                if ( linkState() != Link::CONNECTED ) {
                    std::string text = _( "The connection to the host was lost. Trying to connect again...\n\nRoom code: %{code}" );
                    StringReplace( text, "%{code}", linkText( false ) );
                    return text;
                }

                return std::string( _( "The day is being played out. Please wait..." ) );
            },
            false );

        if ( result != WaitResult::DONE ) {
            return result;
        }

        return loadSnapshotFromHost() ? WaitResult::DONE : WaitResult::PEER_LEFT;
    }

    bool loadSnapshotFromHost()
    {
        const std::string path = snapshotPath();

        if ( !writeFileBytes( path, receivedSnapshot ) ) {
            return false;
        }

        Settings & conf = Settings::Get();
        conf.SetGameType( Game::TYPE_NETWORK );

        const fheroes2::GameMode mode = Game::Load( path );
        std::remove( path.c_str() );

        if ( mode == fheroes2::GameMode::CANCEL ) {
            return false;
        }

        Game::SetLastSaveName( "" );

        guestLoadedDay = receivedSnapshotDay;
        lastOutgoing.clear();
        return true;
    }

    void leaveSession( const std::string & reason )
    {
        if ( sessionRole != Role::NONE ) {
            const std::vector<uint8_t> payload( reason.begin(), reason.end() );

            if ( sessionRole == Role::HOST ) {
                for ( int slot = 0; slot < maxGuests; ++slot ) {
                    sendToSlot( slot, MSG_LEAVE, 0, payload );
                }
            }
            else {
                sendToHost( MSG_LEAVE, 0, payload, false );
            }
        }

        sessionRole = Role::NONE;
        Settings::Get().SetGameType( Settings::Get().GameType() & ~Game::TYPE_NETWORK );
        hostColor = PlayerColor::NONE;
        myColor = PlayerColor::NONE;
        guestSlots = {};
        hostSnapshot.clear();
        receivedSnapshot.clear();
        lastOutgoing.clear();
        seatsAreFinal = false;
        isReloading = false;

        // The page closes the links a moment later, so the notes above can still leave.
        linkClose();
    }

    namespace
    {
        void resetSessionData()
        {
            guestSlots = {};
            hostSnapshot.clear();
            hostSnapshotDay = 0;
            seatsAreFinal = false;
            seatTable = SeatTable();
            seatTableReceived = false;
            seatTableChanged = false;
            receivedSnapshot.clear();
            receivedSnapshotDay = 0;
            guestLoadedDay = 0;
            dayStartHeroIds.clear();
            lastOutgoing.clear();
            lastSeenConnectCount = 0;
            peerLeft = false;
            peerLeftMessage.clear();
            hostColor = PlayerColor::NONE;
            myColor = PlayerColor::NONE;
            isReloading = false;
        }

        int connectedGuests()
        {
            int count = 0;
            for ( int slot = 0; slot < knownSlots(); ++slot ) {
                if ( linkSlotState( slot ) == 1 ) {
                    ++count;
                }
            }

            return count;
        }
    }

    RemoteAsComputer::RemoteAsComputer()
    {
        if ( sessionRole != Role::HOST ) {
            return;
        }

        // The cached set of human colours must not lose the guests while they are treated as computers.
        Players::HumanColors();

        for ( const PlayerColor color : remoteColors() ) {
            Player * player = Players::Get( color );
            if ( player != nullptr ) {
                _players.emplace_back( player, player->GetControl() );
                player->SetControl( CONTROL_AI );
            }
        }
    }

    RemoteAsComputer::~RemoteAsComputer()
    {
        for ( const auto & [player, control] : _players ) {
            player->SetControl( control );
        }
    }

    bool runSeatSelectionAsHost()
    {
        if ( sessionRole != Role::HOST ) {
            return true;
        }

        // From now on nobody new may join; everybody who is linked takes a seat.
        linkLockJoins();
        pumpMessages();

        std::vector<int> activeSlots;
        for ( int slot = 0; slot < knownSlots() && slot < maxGuests; ++slot ) {
            if ( linkSlotState( slot ) == 1 ) {
                activeSlots.push_back( slot );
            }
            else {
                linkDropSlot( slot );
            }
        }

        fillSeatTableFromMap();

        int availableColors = 0;
        for ( int i = 0; i < colorCount; ++i ) {
            availableColors += seatTable.isAvailable[i] ? 1 : 0;
        }

        // More players than seats (cannot normally happen: the map list was filtered for the number of players): the last ones are sent away.
        while ( static_cast<int>( activeSlots.size() ) + 1 > availableColors && !activeSlots.empty() ) {
            const int slot = activeSlots.back();
            activeSlots.pop_back();
            sendToSlot( slot, MSG_LEAVE, 0, { 'n', 'o', ' ', 's', 'e', 'a', 't' } );
            linkDropSlot( slot );
        }

        for ( const int slot : activeSlots ) {
            guestSlots[slot].isActive = true;
        }

        assignFirstSeats( activeSlots );

        Game::setDisplayFadeIn();
        fheroes2::drawMainMenuScreen();
        fheroes2::validateFadeInAndRender();

        const SeatResult result = runSeatDialog( true );

        if ( result != SeatResult::FINISHED ) {
            leaveSession( "the host left" );
            return false;
        }

        // Put the choices into the players of the game, so the map loads with them.
        for ( int i = 0; i < colorCount; ++i ) {
            if ( !seatTable.isAvailable[i] ) {
                continue;
            }

            Player * player = Players::Get( colorFromIndex( i ) );
            if ( player == nullptr ) {
                continue;
            }

            if ( seatTable.owner[i] != ownerFree ) {
                player->SetControl( CONTROL_HUMAN );
                player->SetRace( seatTable.race[i] );
            }
            else {
                player->SetControl( CONTROL_AI );
            }

            if ( seatTable.owner[i] == ownerHost ) {
                hostColor = colorFromIndex( i );
            }
            else if ( seatTable.owner[i] < maxGuests ) {
                guestSlots[seatTable.owner[i]].color = colorFromIndex( i );
            }
        }

        // A guest who could not be placed does not play.
        for ( GuestSlot & guest : guestSlots ) {
            if ( guest.color == PlayerColor::NONE ) {
                guest.isActive = false;
            }
        }

        seatTable.isFinal = true;
        seatsAreFinal = true;

        for ( int slot = 0; slot < maxGuests; ++slot ) {
            if ( guestSlots[slot].isActive ) {
                sendToSlot( slot, MSG_SEATS, 0, encodeSeatTable( seatTable, static_cast<uint8_t>( slot ) ) );
            }
        }

        fheroes2::fadeOutDisplay();
        return true;
    }

    fheroes2::GameMode runLobby()
    {
        if ( !isAvailable() ) {
            fheroes2::showStandardTextMessage( _( "Online game" ), _( "Online play works in the browser version of the game only." ), Dialog::OK );
            return fheroes2::GameMode::MAIN_MENU;
        }

        enum class State : uint8_t
        {
            CHOOSE,
            HOST,
            JOIN
        };

        State state = State::CHOOSE;

        fheroes2::Display & display = fheroes2::Display::instance();
        LocalEvent & le = LocalEvent::Get();

        fheroes2::drawMainMenuScreen();
        display.render();

        const CursorRestorer cursorRestorer( true, Cursor::POINTER );

        std::unique_ptr<DialogWindow> window = std::make_unique<DialogWindow>( 440, 250, _( "Online game" ), _( "CANCEL" ) );

        // The choose state is plain text: H = host, J = join.
        window->setBody(
            _( "Play with friends over the internet.\n\nPress H to host a game (you get a room code).\nPress J to join a game (you type the room code).\n\nUp to six human players." ) );

        resetSessionData();

        bool isSeatDialogDone = false;

        while ( true ) {
            if ( !le.HandleEvents( true, true ) ) {
                if ( Game::processExitEvent() == fheroes2::GameMode::QUIT_GAME ) {
                    leaveSession( "" );
                    return fheroes2::GameMode::QUIT_GAME;
                }

                continue;
            }

            window->button().drawOnState( le.isMouseLeftButtonPressedAndHeldInArea( window->button().area() ) );
            window->renderAll();

            if ( le.MouseClickLeft( window->button().area() ) || Game::HotKeyPressEvent( Game::HotKeyEvent::DEFAULT_CANCEL ) ) {
                leaveSession( "" );
                return fheroes2::GameMode::MAIN_MENU;
            }

            if ( state == State::CHOOSE ) {
                if ( le.isKeyPressed( fheroes2::Key::KEY_H ) ) {
                    state = State::HOST;
                    sessionRole = Role::HOST;
                    Settings::Get().SetGameType( Game::TYPE_NETWORK );
                    linkHost();
                }
                else if ( le.isKeyPressed( fheroes2::Key::KEY_J ) ) {
                    std::string code;
                    if ( Dialog::inputString( fheroes2::Text( _( "Join a game" ), fheroes2::FontType::normalYellow() ),
                                              fheroes2::Text( _( "Type the room code of the host:" ), fheroes2::FontType::normalWhite() ), code, 5, false,
                                              std::nullopt )
                         && !code.empty() ) {
                        state = State::JOIN;
                        sessionRole = Role::GUEST;
                        Settings::Get().SetGameType( Game::TYPE_NETWORK );
                        linkJoin( code );
                    }

                    le.reset();
                }

                continue;
            }

            pumpMessages();

            const Link link = linkState();

            if ( link == Link::FAILED ) {
                window->setBody( linkText( true ) + "\n\n" + _( "Press CANCEL to go back." ) );
                continue;
            }

            if ( state == State::HOST ) {
                if ( link == Link::STARTING ) {
                    window->setBody( _( "Connecting to the free matchmaking service..." ) );
                    continue;
                }

                const int joined = connectedGuests();

                std::string text = _( "Your room code is\n\n" ) + linkText( false ) + "\n\n";

                if ( joined == 0 ) {
                    text += _( "Tell it to your friends. They choose \"Join\" and type it.\n\nNobody has joined yet." );
                }
                else {
                    std::string count = _( "Players joined: %{count} (up to 5 can join).\n\nPress ENTER to choose the map and start." );
                    StringReplace( count, "%{count}", std::to_string( joined ) );
                    text += count;
                }

                window->setBody( text );

                if ( joined > 0 && Game::HotKeyPressEvent( Game::HotKeyEvent::DEFAULT_OKAY ) ) {
                    // The map list is filtered for the number of human players.
                    const int humans = joined + 1;
                    return static_cast<fheroes2::GameMode>( static_cast<int>( fheroes2::GameMode::SELECT_SCENARIO_ONE_HUMAN_PLAYER ) + humans - 1 );
                }
            }
            else {
                if ( link == Link::STARTING || link == Link::CONNECTING ) {
                    window->setBody( _( "Connecting..." ) );
                }
                else if ( link == Link::CONNECTED ) {
                    window->setBody( _( "Connected. Waiting for the host to choose the map..." ) );

                    if ( seatTableReceived && !isSeatDialogDone && receivedSnapshotDay == 0 ) {
                        // The host has chosen the map: everybody chooses a colour and faction.
                        window.reset();
                        const SeatResult result = runSeatDialog( false );
                        isSeatDialogDone = true;

                        if ( result == SeatResult::QUIT || result == SeatResult::LEFT || result == SeatResult::HOST_GONE ) {
                            if ( result == SeatResult::HOST_GONE ) {
                                fheroes2::showStandardTextMessage( _( "Online game" ), _( "The host has left." ), Dialog::OK );
                            }

                            leaveSession( result == SeatResult::HOST_GONE ? "" : "left the game" );
                            return result == SeatResult::QUIT ? fheroes2::GameMode::QUIT_GAME : fheroes2::GameMode::MAIN_MENU;
                        }

                        fheroes2::drawMainMenuScreen();
                        display.render();
                        window = std::make_unique<DialogWindow>( 440, 250, _( "Online game" ), _( "CANCEL" ) );
                        window->setBody( _( "The game is starting. Please wait..." ) );
                    }

                    if ( receivedSnapshotDay > 0 ) {
                        if ( loadSnapshotFromHost() ) {
                            fheroes2::fadeOutDisplay();
                            return fheroes2::GameMode::START_GAME;
                        }

                        fheroes2::showStandardTextMessage( _( "Online game" ), _( "The game sent by the host could not be loaded." ), Dialog::OK );
                        leaveSession( "load failed" );
                        return fheroes2::GameMode::MAIN_MENU;
                    }
                }
                else {
                    window->setBody( _( "Connection lost. Trying to connect again..." ) );
                }

                if ( peerLeft ) {
                    fheroes2::showStandardTextMessage( _( "Online game" ), _( "The host has left." ), Dialog::OK );
                    leaveSession( "" );
                    return fheroes2::GameMode::MAIN_MENU;
                }
            }
        }
    }
}
