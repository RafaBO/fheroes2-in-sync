/***************************************************************************
 *   fheroes2: https://github.com/ihhub/fheroes2                           *
 *   Copyright (C) 2019 - 2025                                             *
 *                                                                         *
 *   Free Heroes2 Engine: http://sourceforge.net/projects/fheroes2         *
 *   Copyright (C) 2009 by Andrey Afletdinov <fheroes2@gmail.com>          *
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

#include "interface_buttons.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <cassert>

#include "dialog.h"
#include "game.h"
#include "game_interface.h"
#include "heroes.h"
#include "icn.h"
#include "interface_base.h"
#include "kingdom.h"
#include "localevent.h"
#include "mp2.h"
#include "route.h"
#include "screen.h"
#include "settings.h"
#include "translations.h"
#include "game_assets.h"
#include "image.h"
#include "ui_dialog.h"
#include "world.h"

namespace
{
    // Owner's 'Undo last stop' icon, drawn inside the 28 x 28 face of the 36 x 36 button. Each entry is x, y, colour (0 = dark outline, 1 = highlight).
    constexpr int undoIconPixelCount = 136;
    constexpr int undoIconPixels[undoIconPixelCount][3] = {
        { 23, 4, 0 },
        { 24, 4, 0 },
        { 30, 4, 0 },
        { 31, 4, 0 },
        { 22, 5, 1 },
        { 23, 5, 1 },
        { 24, 5, 0 },
        { 25, 5, 0 },
        { 29, 5, 0 },
        { 30, 5, 0 },
        { 31, 5, 1 },
        { 24, 6, 1 },
        { 25, 6, 0 },
        { 26, 6, 0 },
        { 28, 6, 0 },
        { 29, 6, 0 },
        { 30, 6, 1 },
        { 20, 7, 0 },
        { 21, 7, 0 },
        { 22, 7, 0 },
        { 24, 7, 1 },
        { 25, 7, 1 },
        { 26, 7, 0 },
        { 27, 7, 0 },
        { 28, 7, 0 },
        { 29, 7, 1 },
        { 20, 8, 1 },
        { 21, 8, 1 },
        { 22, 8, 1 },
        { 26, 8, 0 },
        { 27, 8, 0 },
        { 28, 8, 0 },
        { 17, 9, 1 },
        { 18, 9, 0 },
        { 25, 9, 0 },
        { 26, 9, 0 },
        { 27, 9, 1 },
        { 28, 9, 0 },
        { 29, 9, 0 },
        { 17, 10, 1 },
        { 18, 10, 0 },
        { 24, 10, 0 },
        { 25, 10, 0 },
        { 26, 10, 1 },
        { 27, 10, 1 },
        { 28, 10, 1 },
        { 29, 10, 0 },
        { 30, 10, 0 },
        { 17, 11, 1 },
        { 18, 11, 0 },
        { 23, 11, 0 },
        { 24, 11, 0 },
        { 25, 11, 1 },
        { 28, 11, 1 },
        { 29, 11, 1 },
        { 30, 11, 0 },
        { 31, 11, 0 },
        { 23, 12, 1 },
        { 24, 12, 1 },
        { 29, 12, 1 },
        { 30, 12, 1 },
        { 31, 12, 1 },
        { 17, 14, 0 },
        { 18, 14, 0 },
        { 19, 14, 0 },
        { 16, 15, 0 },
        { 18, 15, 1 },
        { 19, 15, 1 },
        { 20, 15, 0 },
        { 14, 16, 1 },
        { 15, 16, 0 },
        { 20, 16, 1 },
        { 21, 16, 0 },
        { 14, 17, 1 },
        { 15, 17, 0 },
        { 20, 17, 1 },
        { 21, 17, 0 },
        { 14, 18, 1 },
        { 15, 18, 0 },
        { 20, 18, 1 },
        { 21, 18, 0 },
        { 15, 19, 1 },
        { 16, 19, 0 },
        { 20, 19, 0 },
        { 21, 19, 1 },
        { 16, 20, 1 },
        { 17, 20, 0 },
        { 18, 20, 0 },
        { 19, 20, 0 },
        { 20, 20, 1 },
        { 18, 21, 1 },
        { 19, 21, 1 },
        { 17, 23, 1 },
        { 18, 23, 0 },
        { 7, 24, 0 },
        { 8, 24, 0 },
        { 9, 24, 0 },
        { 17, 24, 1 },
        { 18, 24, 0 },
        { 6, 25, 0 },
        { 7, 25, 1 },
        { 8, 25, 1 },
        { 9, 25, 1 },
        { 10, 25, 0 },
        { 17, 25, 1 },
        { 18, 25, 0 },
        { 4, 26, 1 },
        { 5, 26, 0 },
        { 6, 26, 1 },
        { 10, 26, 1 },
        { 11, 26, 0 },
        { 4, 27, 1 },
        { 5, 27, 0 },
        { 10, 27, 1 },
        { 11, 27, 0 },
        { 14, 27, 0 },
        { 15, 27, 0 },
        { 16, 27, 0 },
        { 4, 28, 1 },
        { 5, 28, 0 },
        { 10, 28, 1 },
        { 11, 28, 0 },
        { 14, 28, 1 },
        { 15, 28, 1 },
        { 5, 29, 1 },
        { 6, 29, 0 },
        { 10, 29, 0 },
        { 11, 29, 1 },
        { 6, 30, 1 },
        { 7, 30, 0 },
        { 8, 30, 0 },
        { 9, 30, 0 },
        { 10, 30, 1 },
        { 7, 31, 1 },
        { 8, 31, 1 },
        { 9, 31, 1 } };

    bool isSimultaneousPlanning()
    {
        const Settings & conf = Settings::Get();

        return conf.isSimultaneousTurnsEnabled() && !conf.IsGameType( Game::TYPE_HOTSEAT ) && !conf.IsGameType( Game::TYPE_AUTO_PLAYTEST );
    }

    // Builds a button picture: the original button with its picture wiped out and the undo icon carved on it.
    fheroes2::Sprite makeUndoButton( const fheroes2::Sprite & original, const int32_t offset, const bool isEvilInterface )
    {
        fheroes2::Sprite result( original );

        const int32_t width = result.width();
        const int32_t height = result.height();
        if ( width < 12 || height < 12 ) {
            return result;
        }

        uint8_t * image = result.image();
        uint8_t * transform = result.singleLayer() ? nullptr : result.transform();

        const int32_t minX = 4 + offset;
        const int32_t minY = 4 + offset;
        const int32_t maxX = std::min( width - 4 + offset, width );
        const int32_t maxY = std::min( height - 4 + offset, height );

        std::array<int32_t, 256> histogram{};
        for ( int32_t y = minY; y < maxY; ++y ) {
            for ( int32_t x = minX; x < maxX; ++x ) {
                ++histogram[image[y * width + x]];
            }
        }

        const uint8_t background = static_cast<uint8_t>( std::distance( histogram.begin(), std::max_element( histogram.begin(), histogram.end() ) ) );

        for ( int32_t y = minY; y < maxY; ++y ) {
            for ( int32_t x = minX; x < maxX; ++x ) {
                image[y * width + x] = background;

                if ( transform != nullptr ) {
                    transform[y * width + x] = 0;
                }
            }
        }

        const uint8_t dark = isEvilInterface ? fheroes2::GetColorId( 0x18, 0x18, 0x18 ) : fheroes2::GetColorId( 0x58, 0x28, 0x08 );
        const uint8_t highlight = fheroes2::GetColorId( 0xFC, 0xFC, 0xFC );

        for ( const auto & pixel : undoIconPixels ) {
            const int32_t x = pixel[0] + offset;
            const int32_t y = pixel[1] + offset;

            if ( x >= minX && x < maxX && y >= minY && y < maxY ) {
                image[y * width + x] = pixel[2] != 0 ? highlight : dark;
            }
        }

        return result;
    }
}

void Interface::ButtonsPanel::SavePosition()
{
    Settings & conf = Settings::Get();

    conf.SetPosButtons( GetRect().getPosition() );
    conf.Save( Settings::configFileName );
}

void Interface::ButtonsPanel::setRedraw() const
{
    _interface.setRedraw( REDRAW_BUTTONS );
}

void Interface::ButtonsPanel::SetPos( int32_t x, int32_t y )
{
    BorderWindow::SetPosition( x, y );

    const int icnbtn = Settings::Get().isEvilInterfaceEnabled() ? ICN::ADVEBTNS : ICN::ADVBTNS;

    _buttonNextHero.setICNInfo( icnbtn, 0, 1 );
    _buttonHeroMovement.setICNInfo( icnbtn, 2, 3 );
    _buttonKingdom.setICNInfo( icnbtn, 4, 5 );
    _buttonSpell.setICNInfo( icnbtn, 6, 7 );
    _buttonEndTurn.setICNInfo( icnbtn, 8, 9 );
    _buttonAdventure.setICNInfo( icnbtn, 10, 11 );
    _buttonFile.setICNInfo( icnbtn, 12, 13 );
    _buttonSystem.setICNInfo( icnbtn, 14, 15 );

    _setButtonStatus();

    x = GetArea().x;
    y = GetArea().y;

    // Top row
    _buttonNextHero.setPosition( x, y );
    _nextHeroRect = _buttonNextHero.area();

    _buttonHeroMovement.setPosition( _nextHeroRect.x + _nextHeroRect.width, y );
    _heroMovementRect = _buttonHeroMovement.area();

    _buttonKingdom.setPosition( _heroMovementRect.x + _heroMovementRect.width, y );
    _kingdomRect = _buttonKingdom.area();

    _buttonSpell.setPosition( _kingdomRect.x + _kingdomRect.width, y );
    _spellRect = _buttonSpell.area();

    // Bottom row
    y = _nextHeroRect.y + _nextHeroRect.height;

    _buttonEndTurn.setPosition( x, y );
    _endTurnRect = _buttonEndTurn.area();

    _buttonAdventure.setPosition( _endTurnRect.x + _endTurnRect.width, y );
    _adventureRect = _buttonAdventure.area();

    _buttonFile.setPosition( _adventureRect.x + _adventureRect.width, y );
    _fileRect = _buttonFile.area();

    _buttonSystem.setPosition( _fileRect.x + _fileRect.width, y );
    _systemRect = _buttonSystem.area();
}

void Interface::ButtonsPanel::_redraw()
{
    const Settings & conf = Settings::Get();

    if ( conf.isHideInterfaceEnabled() ) {
        if ( !conf.ShowButtons() ) {
            return;
        }

        BorderWindow::Redraw();
    }

    _setButtonStatus();

    _buttonNextHero.draw();
    _buttonHeroMovement.draw();

    if ( isSimultaneousPlanning() ) {
        // Simultaneous turns: this button always shows the undo icon. It removes the last stop of the route.
        const bool isEvilInterface = conf.isEvilInterfaceEnabled();
        const bool isPressed = _buttonHeroMovement.isEnabled() && _buttonHeroMovement.isPressed();
        const fheroes2::Sprite undoButton
            = makeUndoButton( Assets::getImage( isEvilInterface ? ICN::ADVEBTNS : ICN::ADVBTNS, isPressed ? 3 : 2 ), isPressed ? 1 : 0, isEvilInterface );

        fheroes2::Blit( undoButton, fheroes2::Display::instance(), _heroMovementRect.x, _heroMovementRect.y );
    }

    _buttonKingdom.draw();
    _buttonSpell.draw();
    _buttonEndTurn.draw();
    _buttonAdventure.draw();
    _buttonFile.draw();
    _buttonSystem.draw();
}

fheroes2::GameMode Interface::ButtonsPanel::queueEventProcessing()
{
    captureMouse();

    LocalEvent & le = LocalEvent::Get();

    // In the "no interface" mode, the buttons panel may be overlapped by other UI elements, so we can't render the
    // pressed or released buttons exclusively in a usual way. The overlapping UI elements should also be rendered.
    const auto drawOnPressOrRelease = [this, &le]( fheroes2::Button & button, const fheroes2::Rect & buttonRect ) {
        bool shouldRedraw = false;

        if ( le.isMouseLeftButtonPressedInArea( buttonRect ) ) {
            if ( !button.isPressed() && button.press() ) {
                shouldRedraw = true;
            }
        }
        else {
            if ( !button.isReleased() && button.release() ) {
                shouldRedraw = true;
            }
        }

        if ( shouldRedraw ) {
            _interface.redraw( REDRAW_BUTTONS );

            // Render now only the changed part of the screen.
            fheroes2::Display::instance().render( buttonRect );
        }
    };

    drawOnPressOrRelease( _buttonNextHero, _nextHeroRect );
    drawOnPressOrRelease( _buttonHeroMovement, _heroMovementRect );
    drawOnPressOrRelease( _buttonKingdom, _kingdomRect );
    drawOnPressOrRelease( _buttonSpell, _spellRect );
    drawOnPressOrRelease( _buttonEndTurn, _endTurnRect );
    drawOnPressOrRelease( _buttonAdventure, _adventureRect );
    drawOnPressOrRelease( _buttonFile, _fileRect );
    drawOnPressOrRelease( _buttonSystem, _systemRect );

    fheroes2::GameMode res = fheroes2::GameMode::CANCEL;

    // Move the window border.
    if ( Settings::Get().ShowButtons() && BorderWindow::QueueEventProcessing() ) {
        setRedraw();
    }
    else if ( _buttonNextHero.isEnabled() && le.MouseClickLeft( _nextHeroRect ) ) {
        _interface.EventNextHero();
    }
    else if ( _buttonHeroMovement.isEnabled() && le.MouseClickLeft( _heroMovementRect ) ) {
        res = _interface.EventHeroMovement();
    }
    // The 'MouseLongPressLeft()' for '_heroMovementRect' should be called only if current hero
    // has valid path not to trigger the '_mouseButtonLongPressDelay'.
    // The inlined 'isMouseLeftButtonPressedInArea()' method is called not to call 'isValidForMovement()'
    // method every time this button is enabled.
    else if ( _buttonHeroMovement.isEnabled() && le.isMouseLeftButtonPressedInArea( _heroMovementRect ) &&
              []() {
                  const Heroes * currentHero = GetFocusHeroes();
                  assert( currentHero != nullptr );

                  return currentHero->GetPath().isValidForMovement();
              }()
              && le.MouseLongPressLeft( _heroMovementRect ) ) {
        _interface.EventResetHeroPath();
    }
    else if ( le.MouseClickLeft( _kingdomRect ) ) {
        _interface.EventKingdomInfo();
    }
    else if ( _buttonSpell.isEnabled() && le.MouseClickLeft( _spellRect ) ) {
        _interface.EventCastSpell();
    }
    else if ( le.MouseClickLeft( _endTurnRect ) ) {
        res = _interface.EventEndTurn();
    }
    else if ( le.MouseClickLeft( _adventureRect ) ) {
        res = _interface.EventAdventureDialog();
    }
    else if ( le.MouseClickLeft( _fileRect ) ) {
        res = _interface.EventFileDialog();
    }
    else if ( le.MouseClickLeft( _systemRect ) ) {
        _interface.EventSystemDialog();
    }
    else if ( le.isMouseRightButtonPressedInArea( _nextHeroRect ) ) {
        fheroes2::showStandardTextMessage( _( "Next Hero" ), _( "Select the next Hero." ), Dialog::ZERO );
    }
    else if ( le.isMouseRightButtonPressedInArea( _heroMovementRect ) && isSimultaneousPlanning() ) {
        fheroes2::showStandardTextMessage( _( "Undo Last Stop" ), _( "Remove the last stop of the hero's route. Press and hold this button to remove the whole route." ), Dialog::ZERO );
    }
    else if ( le.isMouseRightButtonPressedInArea( _heroMovementRect ) ) {
        fheroes2::showStandardTextMessage(
            _( "Hero Movement" ),
            _( "Start the Hero's movement along the current path or re-visit the object occupied by the Hero. Press and hold this button to reset the Hero's path." ),
            Dialog::ZERO );
    }
    else if ( le.isMouseRightButtonPressedInArea( _kingdomRect ) ) {
        fheroes2::showStandardTextMessage( _( "Kingdom Summary" ), _( "View a summary of your Kingdom." ), Dialog::ZERO );
    }
    else if ( le.isMouseRightButtonPressedInArea( _spellRect ) ) {
        fheroes2::showStandardTextMessage( _( "Cast Spell" ), _( "Cast an adventure spell." ), Dialog::ZERO );
    }
    else if ( le.isMouseRightButtonPressedInArea( _endTurnRect ) ) {
        fheroes2::showStandardTextMessage( _( "End Turn" ), _( "End your turn and let the computer take its turn." ), Dialog::ZERO );
    }
    else if ( le.isMouseRightButtonPressedInArea( _adventureRect ) ) {
        fheroes2::showStandardTextMessage( _( "Adventure Options" ), _( "Bring up the adventure options menu." ), Dialog::ZERO );
    }
    else if ( le.isMouseRightButtonPressedInArea( _fileRect ) ) {
        fheroes2::showStandardTextMessage( _( "File Options" ), _( "Bring up the file options menu, allowing you to load, save, start a new game or quit." ),
                                           Dialog::ZERO );
    }
    else if ( le.isMouseRightButtonPressedInArea( _systemRect ) ) {
        fheroes2::showStandardTextMessage( _( "System Options" ), _( "Bring up the system options menu, allowing you to customize your game." ), Dialog::ZERO );
    }

    return res;
}

void Interface::ButtonsPanel::_setButtonStatus()
{
    Heroes * currentHero = GetFocusHeroes();

    if ( isSimultaneousPlanning() ) {
        // Undo button: active only while the hero has a plotted route.
        _buttonHeroMovement.setICNIndexes( 2, 3 );

        if ( currentHero && currentHero->GetPath().isValidForMovement() ) {
            _buttonHeroMovement.enable();
        }
        else {
            _buttonHeroMovement.disable();
        }
    }
    else if ( currentHero && currentHero->GetPath().isValidForMovement() && currentHero->MayStillMove( false, true ) ) {
        // Move button: to move the hero by the specified path.
        _buttonHeroMovement.setICNIndexes( 2, 3 );
        _buttonHeroMovement.enable();
    }
    else if ( currentHero && MP2::isRevisitAllowedForObject( currentHero->getObjectTypeUnderHero(), currentHero->isShipMaster() ) ) {
        // Action button: to visit the action object hero is standing on.
        _buttonHeroMovement.setICNIndexes( 16, 17 );
        _buttonHeroMovement.enable();
    }
    else if ( currentHero && currentHero->GetPath().isValidForMovement() ) {
        // Inactive button: to show the hero's path can be reset.
        _buttonHeroMovement.setICNIndexes( 18, 19 );
        _buttonHeroMovement.enable();
    }
    else {
        // Inactive button (disabled): to show that the hero as no path specified.
        _buttonHeroMovement.setICNIndexes( 18, 19 );
        _buttonHeroMovement.disable();
    }

    if ( currentHero && currentHero->HaveSpellBook() && currentHero->MayCastAdventureSpells() ) {
        _buttonSpell.enable();
    }
    else {
        _buttonSpell.disable();
    }

    const Kingdom & kingdom = world.GetKingdom( Settings::Get().CurrentColor() );
    const VecHeroes & heroes = kingdom.GetHeroes();

    const bool isMovableHeroPresent = std::any_of( heroes.begin(), heroes.end(), []( const Heroes * hero ) {
        assert( hero != nullptr );

        return hero->MayStillMove( false, false );
    } );

    if ( isMovableHeroPresent ) {
        _buttonNextHero.enable();
    }
    else {
        _buttonNextHero.disable();
    }
}
