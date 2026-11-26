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
#include "cursor.h"
#include "dialog.h"
#include "game.h"
#include "game_exit.h"
#include "game_hotkeys.h"
#include "game_io.h"
#include "game_mainmenu_ui.h"
#include "heroes.h"
#include "image.h"
#include "kingdom.h"
#include "localevent.h"
#include "logging.h"
#include "maps.h"
#include "maps_tiles.h"
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
    // ----- The link (implemented by online.js in the page) -----

    enum MessageType : uint8_t
    {
        MSG_HELLO = 1,
        MSG_SNAPSHOT = 2,
        MSG_PLAN = 3,
        MSG_LEAVE = 4
    };

    constexpr uint8_t protocolVersion = 1;

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

    bool linkSend( const std::vector<uint8_t> & data )
    {
        return EM_ASM_INT( {
            var bytes = HEAPU8.slice( $0, $0 + $1 );
            return window.fhNet.send( bytes ) ? 1 : 0;
        }, data.data(), static_cast<int>( data.size() ) ) != 0;
    }

    bool linkTake( std::vector<uint8_t> & data )
    {
        const int size = EM_ASM_INT( { return ( typeof window.fhNet !== 'undefined' ) ? window.fhNet.nextSize() : -1; } );
        if ( size < 0 ) {
            return false;
        }

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

    std::string linkText( const bool )
    {
        return {};
    }

    bool linkSend( const std::vector<uint8_t> & )
    {
        return false;
    }

    bool linkTake( std::vector<uint8_t> & )
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

    Online::Role sessionRole{ Online::Role::NONE };
    PlayerColor hostColor{ PlayerColor::NONE };
    PlayerColor guestColor{ PlayerColor::NONE };

    bool isReloading{ false };

    // Both sides: the last message that matters for the current step, sent again when the link comes back.
    std::vector<uint8_t> lastOutgoing;
    uint32_t lastSeenConnectCount{ 0 };

    // Host side
    std::vector<uint8_t> hostSnapshot;
    uint32_t hostSnapshotDay{ 0 };
    std::vector<uint8_t> receivedPlan;
    uint32_t receivedPlanDay{ 0 };
    uint32_t mergedPlanDay{ 0 };

    // Guest side
    std::vector<uint8_t> receivedSnapshot;
    uint32_t receivedSnapshotDay{ 0 };
    uint32_t guestLoadedDay{ 0 };
    std::vector<int> dayStartHeroIds;

    bool peerLeft{ false };
    std::string peerLeftMessage;

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

    // Sends the message and remembers it as the one to repeat when the link comes back (only for the messages the other side waits for).
    bool sendMessage( const uint8_t type, const uint32_t day, const std::vector<uint8_t> & payload, const bool rememberForResend )
    {
        std::vector<uint8_t> data = encode( type, day, payload );
        if ( rememberForResend ) {
            lastOutgoing = data;
        }

        return linkSend( data );
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

    // ----- The plan the guest sends (the parts of the game that belong to the guest) -----

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

        const PlayerColor color = guestColor;
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

    // Merges the guest's plan into the host's game. Returns false if the plan could not be read.
    bool applyGuestPlan( const std::vector<uint8_t> & bytes )
    {
        Game::SetVersionOfCurrentSaveFile( CURRENT_FORMAT_VERSION );

        const PlayerColor color = guestColor;
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

            // A hero that the host's game gave to somebody else in the meantime cannot be taken (both hired the same hero on the same day).
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

    // ----- Receiving -----

    void handleMessage( const Message & message )
    {
        switch ( message.type ) {
        case MSG_LEAVE:
            peerLeft = true;
            peerLeftMessage.assign( message.payload.begin(), message.payload.end() );
            break;
        case MSG_HELLO:
            if ( sessionRole == Online::Role::HOST && !hostSnapshot.empty() && message.day != hostSnapshotDay ) {
                // The guest is behind (it has just joined or came back after losing its game): give it today's snapshot.
                linkSend( encode( MSG_SNAPSHOT, hostSnapshotDay, hostSnapshot ) );
            }
            break;
        case MSG_SNAPSHOT:
            if ( sessionRole == Online::Role::GUEST && message.day > receivedSnapshotDay ) {
                receivedSnapshot = message.payload;
                receivedSnapshotDay = message.day;
            }
            break;
        case MSG_PLAN:
            if ( sessionRole == Online::Role::HOST && message.day > receivedPlanDay ) {
                receivedPlan = message.payload;
                receivedPlanDay = message.day;
            }
            break;
        default:
            break;
        }
    }

    // Reads everything that has arrived and repeats the last message after the link came back.
    void pumpMessages()
    {
        const uint32_t connectCount = linkConnectCount();
        if ( connectCount != lastSeenConnectCount ) {
            lastSeenConnectCount = connectCount;

            if ( connectCount > 0 && sessionRole == Online::Role::GUEST ) {
                linkSend( encode( MSG_HELLO, guestLoadedDay, { protocolVersion } ) );

                if ( !lastOutgoing.empty() ) {
                    linkSend( lastOutgoing );
                }
            }
        }

        std::vector<uint8_t> raw;
        while ( linkTake( raw ) ) {
            Message message;
            if ( decode( std::move( raw ), message ) ) {
                handleMessage( message );
            }
            raw.clear();
        }
    }

    // ----- Dialogs -----

    class DialogWindow
    {
    public:
        DialogWindow( const int32_t width, const int32_t height, const std::string & title, const char * buttonText )
            : _display( fheroes2::Display::instance() )
            , _restorer( _display, ( _display.width() - width ) / 2 - 16, ( _display.height() - height ) / 2 - 16, width + 32, height + 32 )
            , _window( width, height, true, _display )
        {
            const fheroes2::Rect & area = _window.activeArea();

            fheroes2::Text titleText( title, fheroes2::FontType::normalYellow() );
            titleText.draw( area.x + 10, area.y + 12, area.width - 20, _display );

            _bodyArea = { area.x + 15, area.y + 40, area.width - 30, area.height - 40 - 50 };
            _bodyRestorer = std::make_unique<fheroes2::ImageRestorer>( _display, _bodyArea.x, _bodyArea.y, _bodyArea.width, _bodyArea.height );

            _window.renderTextAdaptedButtonSprite( _button, buttonText, { 0, 10 }, fheroes2::StandardWindow::Padding::BOTTOM_CENTER );

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
        std::string _lastBody;
    };

    // Shows a small window until the condition is true. While the link is down the text says the game is paused. Returns how the wait ended.
    Online::WaitResult waitForEvent( const std::function<bool()> & isDone, const std::function<std::string()> & makeText )
    {
        pumpMessages();

        if ( isDone() ) {
            return Online::WaitResult::DONE;
        }

        if ( peerLeft ) {
            return Online::WaitResult::PEER_LEFT;
        }

        const CursorRestorer cursorRestorer( true, Cursor::POINTER );

        DialogWindow dialog( 400, 190, _( "Online game" ), _( "LEAVE" ) );
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
            dialog.renderAll();

            if ( le.MouseClickLeft( dialog.button().area() ) ) {
                if ( fheroes2::showStandardTextMessage( _( "Leave game" ), _( "Do you really want to leave the online game? The other player will be told." ),
                                                       Dialog::YES | Dialog::NO )
                     == Dialog::YES ) {
                    return Online::WaitResult::LEFT;
                }
            }
        }
    }

    std::string linkDownText( const std::string & whoIsGone )
    {
        std::string text = _( "The connection to %{player} was lost. The game is paused.\n\nThe game goes on as soon as %{player} is back. Room code: %{code}" );
        StringReplace( text, "%{player}", whoIsGone );
        StringReplace( text, "%{code}", linkText( false ) );
        return text;
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
        return sessionRole == Role::GUEST ? guestColor : hostColor;
    }

    PlayerColor remoteColor()
    {
        return sessionRole == Role::GUEST ? hostColor : guestColor;
    }

    bool isRemoteColor( const PlayerColor color )
    {
        return isNetworkGame() && color != PlayerColor::NONE && color == remoteColor();
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

    bool isPeerLinkDown()
    {
        if ( !isNetworkGame() ) {
            return false;
        }

        const Link state = linkState();
        return state != Link::CONNECTED;
    }

    void assignColors()
    {
        std::vector<PlayerColor> humans;
        for ( const Player * player : Settings::Get().GetPlayers() ) {
            if ( player != nullptr && player->isControlHuman() ) {
                humans.push_back( player->GetColor() );
            }
        }

        std::sort( humans.begin(), humans.end(), []( const PlayerColor a, const PlayerColor b ) { return static_cast<int>( a ) < static_cast<int>( b ); } );

        if ( humans.size() >= 2 ) {
            hostColor = humans[0];
            guestColor = humans[1];
        }
        else {
            // Not an online game after all (wrong map set up): nothing to do.
            hostColor = PlayerColor::NONE;
            guestColor = PlayerColor::NONE;
        }

        DEBUG_LOG( DBG_GAME, DBG_INFO, "online: host colour " << Color::String( hostColor ) << ", guest colour " << Color::String( guestColor ) )
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

        // If the link is down the snapshot goes out when the guest comes back and says hello.
        linkSend( encode( MSG_SNAPSHOT, hostSnapshotDay, hostSnapshot ) );
        lastSeenConnectCount = linkConnectCount();

        return true;
    }

    WaitResult hostCollectGuestPlan()
    {
        assert( sessionRole == Role::HOST );

        const uint32_t day = world.CountDay();

        const WaitResult result = waitForEvent(
            [day]() { return receivedPlanDay >= day; },
            [day]() {
                if ( linkState() != Link::CONNECTED ) {
                    return linkDownText( _( "the other player" ) );
                }

                return std::string( _( "Waiting for the other player to finish planning the day..." ) );
            } );

        if ( result != WaitResult::DONE ) {
            return result;
        }

        if ( mergedPlanDay < day ) {
            if ( !applyGuestPlan( receivedPlan ) ) {
                DEBUG_LOG( DBG_GAME, DBG_WARN, "online: the other player's plan could not be read" )
            }

            mergedPlanDay = day;
        }

        return WaitResult::DONE;
    }

    void guestBeginDay()
    {
        dayStartHeroIds.clear();

        for ( const Heroes * hero : world.GetKingdom( guestColor ).GetHeroes() ) {
            dayStartHeroIds.push_back( hero->GetID() );
        }

        lastOutgoing.clear();
    }

    WaitResult guestSendPlanAndLoadNextDay()
    {
        assert( sessionRole == Role::GUEST );

        const uint32_t day = world.CountDay();

        sendMessage( MSG_PLAN, day, buildGuestPlan(), true );
        lastSeenConnectCount = linkConnectCount();

        const WaitResult result = waitForEvent(
            [day]() { return receivedSnapshotDay > day; },
            []() {
                if ( linkState() != Link::CONNECTED ) {
                    return linkDownText( _( "the host" ) );
                }

                return std::string( _( "The day is being played out. Please wait..." ) );
            } );

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
            linkSend( encode( MSG_LEAVE, 0, payload ) );
        }

        sessionRole = Role::NONE;
        Settings::Get().SetGameType( Settings::Get().GameType() & ~Game::TYPE_NETWORK );
        hostColor = PlayerColor::NONE;
        guestColor = PlayerColor::NONE;
        hostSnapshot.clear();
        receivedPlan.clear();
        receivedSnapshot.clear();
        lastOutgoing.clear();
        isReloading = false;

        // Give the message a moment to leave before the link is closed.
        linkClose();
    }

    namespace
    {
        void resetSessionData()
        {
            hostSnapshot.clear();
            hostSnapshotDay = 0;
            receivedPlan.clear();
            receivedPlanDay = 0;
            mergedPlanDay = 0;
            receivedSnapshot.clear();
            receivedSnapshotDay = 0;
            guestLoadedDay = 0;
            dayStartHeroIds.clear();
            lastOutgoing.clear();
            lastSeenConnectCount = 0;
            peerLeft = false;
            peerLeftMessage.clear();
            hostColor = PlayerColor::NONE;
            guestColor = PlayerColor::NONE;
            isReloading = false;
        }
    }

    RemoteAsComputer::RemoteAsComputer()
    {
        if ( sessionRole != Role::HOST || guestColor == PlayerColor::NONE ) {
            return;
        }

        // The cached set of human colours must not lose the guest while it is treated as a computer.
        Players::HumanColors();

        _player = Players::Get( guestColor );
        if ( _player != nullptr ) {
            _control = _player->GetControl();
            _player->SetControl( CONTROL_AI );
        }
    }

    RemoteAsComputer::~RemoteAsComputer()
    {
        if ( _player != nullptr ) {
            _player->SetControl( _control );
        }
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

        std::unique_ptr<DialogWindow> window;

        // Two buttons are needed in the first two states, so the window is built with a second button next to the first one.
        const auto buildWindow = [&]( const char * firstButton ) { window = std::make_unique<DialogWindow>( 420, 230, _( "Online game" ), firstButton ); };

        buildWindow( _( "CANCEL" ) );

        // The choose state is drawn as plain text with three clickable lines to keep the screen simple: H = host, J = join.
        window->setBody( _( "Play with a friend over the internet.\n\nPress H to host a game (you get a room code).\nPress J to join a game (you type the room code).\n\nOnly two human players are supported." ) );

        resetSessionData();

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
                window->setBody( linkText( true ) + "\n\n" + _( "Press Cancel to go back." ) );
                continue;
            }

            if ( state == State::HOST ) {
                if ( link == Link::STARTING ) {
                    window->setBody( _( "Connecting to the free matchmaking service..." ) );
                }
                else if ( link == Link::WAITING ) {
                    window->setBody( _( "Your room code is\n\n" ) + linkText( false ) + _( "\n\nTell it to the other player. They choose \"Join\" and type it." ), false );
                }
                else if ( link == Link::CONNECTED ) {
                    window->setBody( _( "The other player has joined.\n\nPress ENTER to choose the map and start." ) );

                    if ( Game::HotKeyPressEvent( Game::HotKeyEvent::DEFAULT_OKAY ) ) {
                        Settings & conf = Settings::Get();
                        conf.SetGameType( Game::TYPE_NETWORK );
                        return fheroes2::GameMode::SELECT_SCENARIO_TWO_HUMAN_PLAYERS;
                    }
                }
                else {
                    window->setBody( _( "The other player has left. Waiting for them to join again with the same code:\n\n" ) + linkText( false ) );
                }
            }
            else {
                if ( link == Link::STARTING || link == Link::CONNECTING ) {
                    window->setBody( _( "Connecting..." ) );
                }
                else if ( link == Link::CONNECTED ) {
                    window->setBody( _( "Connected. Waiting for the host to choose the map and start the game..." ) );

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
