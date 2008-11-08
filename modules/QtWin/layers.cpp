/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 1999, 2000 Matthias Ettrich <ettrich@kde.org>
Copyright (C) 2003 Lubos Lunak <l.lunak@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/

// SELI zmenit doc

/*

 This file contains things relevant to stacking order and layers.

 Design:

 Normal unconstrained stacking order, as requested by the user (by clicking
 on windows to raise them, etc.), is in Workspace::unconstrained_stacking_order.
 That list shouldn't be used at all, except for building
 Workspace::stacking_order. The building is done
 in Workspace::constrainedStackingOrder(). Only Workspace::stackingOrder() should
 be used to get the stacking order, because it also checks the stacking order
 is up to date.
 All clients are also stored in Workspace::clients (except for isDesktop() clients,
 as those are very special, and are stored in Workspace::desktops), in the order
 the clients were created.

 Every window has one layer assigned in which it is. There are 6 layers,
 from bottom : DesktopLayer, BelowLayer, NormalLayer, DockLayer, AboveLayer
 and ActiveLayer (see also NETWM sect.7.10.). The layer a window is in depends
 on the window type, and on other things like whether the window is active.

 NET::Splash clients belong to the Normal layer. NET::TopMenu clients
 belong to Dock layer. Clients that are both NET::Dock and NET::KeepBelow
 are in the Normal layer in order to keep the 'allow window to cover
 the panel' Kicker setting to work as intended (this may look like a slight
 spec violation, but a) I have no better idea, b) the spec allows adjusting
 the stacking order if the WM thinks it's a good idea . We put all
 NET::KeepAbove above all Docks too, even though the spec suggests putting
 them in the same layer.

 Most transients are in the same layer as their mainwindow,
 see Workspace::constrainedStackingOrder(), they may also be in higher layers, but
 they should never be below their mainwindow.

 When some client attribute changes (above/below flag, transiency...),
 Workspace::updateClientLayer() should be called in order to make
 sure it's moved to the appropriate layer ClientList if needed.

 Currently the things that affect client in which layer a client
 belongs: KeepAbove/Keep Below flags, window type, fullscreen
 state and whether the client is active, mainclient (transiency).

 Make sure updateStackingOrder() is called in order to make
 Workspace::stackingOrder() up to date and propagated to the world.
 Using Workspace::blockStackingUpdates() (or the StackingUpdatesBlocker
 helper class) it's possible to temporarily disable updates
 and the stacking order will be updated once after it's allowed again.

*/

#include <assert.h>

#include <kdebug.h>

#include "utils.h"
#include "client.h"
#include "workspace.h"
#include "tabbox.h"
#include "group.h"
#include "rules.h"
#include "unmanaged.h"
#include "deleted.h"
#include <QX11Info>

namespace KWin
{

//*******************************
// Workspace
//*******************************

void Workspace::updateClientLayer( Client* c )
    {
    if( c == NULL )
        return;
    if( c->layer() == c->belongsToLayer())
        return;
    StackingUpdatesBlocker blocker( this );
    c->invalidateLayer(); // invalidate, will be updated when doing restacking
    for( ClientList::ConstIterator it = c->transients().begin();
         it != c->transients().end();
         ++it )
        updateClientLayer( *it );
    }

void Workspace::updateStackingOrder( bool propagate_new_clients )
    {
    if( block_stacking_updates > 0 )
        {
        if( propagate_new_clients )
            blocked_propagating_new_clients = true;
        return;
        }
    ClientList new_stacking_order = constrainedStackingOrder();
    bool changed = ( new_stacking_order != stacking_order || force_restacking );
    force_restacking = false;
    stacking_order = new_stacking_order;
#if 0
    kDebug() << "stacking:" << changed;
    if( changed || propagate_new_clients )
        {
        for( ClientList::ConstIterator it = stacking_order.begin();
             it != stacking_order.end();
             ++it )
            kDebug() << (void*)(*it) << *it << ":" << (*it)->layer();
        }
#endif
    if( changed || propagate_new_clients )
        {
        propagateClients( propagate_new_clients );
        addRepaintFull();
        if( active_client )
            active_client->updateMouseGrab();
        }
    }

/*!
  Propagates the managed clients to the world.
  Called ONLY from updateStackingOrder().
 */
void Workspace::propagateClients( bool propagate_new_clients )
    {
    Window *cl; // MW we should not assume WId and Window to be compatible
                                // when passig pointers around.

    // restack the windows according to the stacking order
    // 1 - supportWindow, 1 - topmenu_space, 8 - electric borders
    Window* new_stack = new Window[ stacking_order.count() + 1 + 1 + 8 ];
    int pos = 0;
    // Stack all windows under the support window. The support window is
    // not used for anything (besides the NETWM property), and it's not shown,
    // but it was lowered after kwin startup. Stacking all clients below
    // it ensures that no client will be ever shown above override-redirect
    // windows (e.g. popups).
    new_stack[ pos++ ] = supportWindow->winId();
    for( int i = 0;
         i < ELECTRIC_COUNT;
         ++i )
        if( electric_windows[ i ] != None )
            new_stack[ pos++ ] = electric_windows[ i ];
    int topmenu_space_pos = 1; // not 0, that's supportWindow !!!
    for ( int i = stacking_order.size() - 1; i >= 0; i-- )
        {
        if( stacking_order.at( i )->hiddenPreview())
            continue;
        new_stack[ pos++ ] = stacking_order.at( i )->frameId();
        if( stacking_order.at( i )->belongsToLayer() >= DockLayer )
            topmenu_space_pos = pos;
        }
    if( topmenu_space != NULL )
        { // make sure the topmenu space is below all topmenus, fullscreens, etc.
        for( int i = pos;
             i > topmenu_space_pos;
             --i )
            new_stack[ i ] = new_stack[ i - 1 ];
        new_stack[ topmenu_space_pos ] = topmenu_space->winId();
        ++pos;
        }
    // when having hidden previews, stack hidden windows below everything else
    // (as far as pure X stacking order is concerned), in order to avoid having
    // these windows that should be unmapped to interfere with other windows
    for ( int i = stacking_order.size() - 1; i >= 0; i-- )
        {
        if( !stacking_order.at( i )->hiddenPreview())
            continue;
        new_stack[ pos++ ] = stacking_order.at( i )->frameId();
        if( stacking_order.at( i )->belongsToLayer() >= DockLayer )
            topmenu_space_pos = pos;
        }
    // TODO isn't it too inefficient to restack always all clients?
    // TODO don't restack not visible windows?
    assert( new_stack[ 0 ] == supportWindow->winId());
    XRestackWindows(display(), new_stack, pos);
    delete [] new_stack;

    if ( propagate_new_clients )
        {
        cl = new Window[ desktops.count() + clients.count()];
        pos = 0;
	// TODO this is still not completely in the map order
        for ( ClientList::ConstIterator it = desktops.begin(); it != desktops.end(); ++it )
            cl[pos++] =  (*it)->window();
        for ( ClientList::ConstIterator it = clients.begin(); it != clients.end(); ++it )
            cl[pos++] =  (*it)->window();
        rootInfo->setClientList( cl, pos );
        delete [] cl;
        }

    cl = new Window[ stacking_order.count()];
    pos = 0;
    for ( ClientList::ConstIterator it = stacking_order.begin(); it != stacking_order.end(); ++it)
        cl[pos++] =  (*it)->window();
    rootInfo->setClientListStacking( cl, pos );
    delete [] cl;
    
    // Make the cached stacking order invalid here, in case we need the new stacking order before we get
    // the matching event, due to X being asynchronous.
    x_stacking_dirty = true; 
    }


/*!
  Returns topmost visible client. Windows on the dock, the desktop
  or of any other special kind are excluded. Also if the window
  doesn't accept focus it's excluded.
 */
// TODO misleading name for this method, too many slightly different ways to use it
Client* Workspace::topClientOnDesktop( int desktop, int screen, bool unconstrained, bool only_normal ) const
    {
// TODO    Q_ASSERT( block_stacking_updates == 0 );
    ClientList list;
    if( !unconstrained )
        list = stacking_order;
    else
        list = unconstrained_stacking_order;
    for( int i = list.size() - 1;
         i >= 0;
         --i )
        {
        if( list.at( i )->isOnDesktop( desktop ) && list.at( i )->isShown( false ))
            {
            if( screen != -1 && list.at( i )->screen() != screen )
                continue;
            if( !only_normal )
                return list.at( i );
            if( list.at( i )->wantsTabFocus() && !list.at( i )->isSpecialWindow())
                return list.at( i );
            }
        }
    return 0;
    }

Client* Workspace::findDesktop( bool topmost, int desktop ) const
    {
// TODO    Q_ASSERT( block_stacking_updates == 0 );
    if( topmost )
        {
        for ( int i = stacking_order.size() - 1; i>=0; i-- )
            {
            if ( stacking_order.at( i )->isOnDesktop( desktop ) && stacking_order.at( i )->isDesktop()
                && stacking_order.at( i )->isShown( true ))
                return stacking_order.at(  i );
            }
        }
    else // bottom-most
        {
        foreach ( Client* c, stacking_order )
            {
            if ( c->isOnDesktop( desktop ) && c->isDesktop()
                && c->isShown( true ))
                return c;
            }
        }
    return NULL;
    }

void Workspace::raiseOrLowerClient( Client *c)
    {
    if (!c) return;
    Client* topmost = NULL;
// TODO    Q_ASSERT( block_stacking_updates == 0 );
    if ( most_recently_raised && stacking_order.contains( most_recently_raised ) &&
         most_recently_raised->isShown( true ) && c->isOnCurrentDesktop())
        topmost = most_recently_raised;
    else
        topmost = topClientOnDesktop( c->isOnAllDesktops() ? currentDesktop() : c->desktop(),
            options->separateScreenFocus ? c->screen() : -1 );

    if( c == topmost)
        lowerClient(c);
    else
        raiseClient(c);
    }


void Workspace::lowerClient( Client* c, bool nogroup )
    {
    if ( !c )
        return;
    if( c->isTopMenu())
        return;

    c->cancelAutoRaise();

    StackingUpdatesBlocker blocker( this );

    unconstrained_stacking_order.removeAll( c );
    unconstrained_stacking_order.prepend( c );
    if( !nogroup && c->isTransient() )
        {
        // lower also all windows in the group, in their reversed stacking order
        ClientList wins = ensureStackingOrder( c->group()->members());
        for( int i = wins.size() - 1;
             i >= 0;
             --i )
            {
            if( wins[ i ] != c )
                lowerClient( wins[ i ], true );
            }
        }

    if ( c == most_recently_raised )
        most_recently_raised = 0;
    }

void Workspace::lowerClientWithinApplication( Client* c )
    {
    if ( !c )
        return;
    if( c->isTopMenu())
        return;

    c->cancelAutoRaise();

    StackingUpdatesBlocker blocker( this );

    unconstrained_stacking_order.removeAll( c );
    bool lowered = false;
    // first try to put it below the bottom-most window of the application
    for( ClientList::Iterator it = unconstrained_stacking_order.begin();
         it != unconstrained_stacking_order.end();
         ++it )
        if( Client::belongToSameApplication( *it, c ))
            {
            unconstrained_stacking_order.insert( it, c );
            lowered = true;
            break;
            }
    if( !lowered )
        unconstrained_stacking_order.prepend( c );
    // ignore mainwindows
    }

void Workspace::raiseClient( Client* c, bool nogroup )
    {
    if ( !c )
        return;
    if( c->isTopMenu())
        return;

    c->cancelAutoRaise();

    StackingUpdatesBlocker blocker( this );

    if( !nogroup && c->isTransient())
        {
        ClientList wins = ensureStackingOrder( c->group()->members());
        foreach( Client* c2, wins )
            if( c2 != c )
                raiseClient( c2, true );
        }

    unconstrained_stacking_order.removeAll( c );
    unconstrained_stacking_order.append( c );

    if( !c->isSpecialWindow())
        {
        most_recently_raised = c;
        pending_take_activity = NULL;
        }
    }

void Workspace::raiseClientWithinApplication( Client* c )
    {
    if ( !c )
        return;
    if( c->isTopMenu())
        return;

    c->cancelAutoRaise();

    StackingUpdatesBlocker blocker( this );
    // ignore mainwindows
    
    // first try to put it above the top-most window of the application
	for ( int i = unconstrained_stacking_order.size() - 1; i>= 0 ; i-- )
        {
        if( unconstrained_stacking_order.at( i ) == c ) // don't lower it just because it asked to be raised
            return;
        if( Client::belongToSameApplication( unconstrained_stacking_order.at(  i ), c ))
            {
            unconstrained_stacking_order.removeAll( c );
            unconstrained_stacking_order.insert( ++i, c ); // insert after the found one
            return;
            }
        }
    }

void Workspace::raiseClientRequest( Client* c, NET::RequestSource src, Time timestamp )
    {
    if( src == NET::FromTool || allowFullClientRaising( c, timestamp ))
        raiseClient( c );
    else
        {
        raiseClientWithinApplication( c );
        c->demandAttention();
        }
    }

void Workspace::lowerClientRequest( Client* c, NET::RequestSource src, Time /*timestamp*/ )
    {
    // If the client has support for all this focus stealing prevention stuff,
    // do only lowering within the application, as that's the more logical
    // variant of lowering when application requests it.
    // No demanding of attention here of course.
    if( src == NET::FromTool || !c->hasUserTimeSupport())
        lowerClient( c );
    else
        lowerClientWithinApplication( c );
    }

void Workspace::restackClientUnderActive( Client* c )
    {
    if( c->isTopMenu())
        return;
    if( !active_client || active_client == c )
        {
        raiseClient( c );
        return;
        }

    assert( unconstrained_stacking_order.contains( active_client ));
    if( Client::belongToSameApplication( active_client, c ))
        { // put it below the active window if it's the same app
        unconstrained_stacking_order.removeAll( c );
        unconstrained_stacking_order.insert( unconstrained_stacking_order.indexOf( active_client ), c );
        }
    else
        { // put in the stacking order below _all_ windows belonging to the active application
        for( ClientList::Iterator it = unconstrained_stacking_order.begin();
             it != unconstrained_stacking_order.end();
             ++it )
            { // TODO ignore topmenus?
            if( Client::belongToSameApplication( active_client, *it ))
                {
                if( *it != c )
                    {
                    unconstrained_stacking_order.removeAll( c );
                    unconstrained_stacking_order.insert( it, c );
                    }
                break;
                }
            }
        }
    assert( unconstrained_stacking_order.contains( c ));
    for( int desktop = 1;
         desktop <= numberOfDesktops();
         ++desktop )
        { // do for every virtual desktop to handle the case of onalldesktop windows
        if( c->wantsTabFocus() && c->isOnDesktop( desktop ) && focus_chain[ desktop ].contains( active_client ))
            {
            if( Client::belongToSameApplication( active_client, c ))
                { // put it after the active window if it's the same app
                focus_chain[ desktop ].removeAll( c );
                focus_chain[ desktop ].insert( focus_chain[ desktop ].indexOf( active_client ), c );
                }
            else
                { // put it in focus_chain[currentDesktop()] after all windows belonging to the active applicationa
                focus_chain[ desktop ].removeAll( c );
                for( int i = focus_chain[ desktop ].size() - 1;
                     i >= 0;
                     --i )
                    {
                    if( Client::belongToSameApplication( active_client, focus_chain[ desktop ].at( i )))
                        {
                        focus_chain[ desktop ].insert( i, c );
                        break;
                        }
                    }
                }
            }
        }
    // the same for global_focus_chain
    if( c->wantsTabFocus() && global_focus_chain.contains( active_client ))
        {
        if( Client::belongToSameApplication( active_client, c ))
            {
            global_focus_chain.removeAll( c );
            global_focus_chain.insert( global_focus_chain.indexOf( active_client ), c );
            }
        else
            {
            global_focus_chain.removeAll( c );
            for ( int i = global_focus_chain.size() - 1;
                  i >= 0;
                  --i )
                {
                if( Client::belongToSameApplication( active_client, global_focus_chain.at( i ) ))
                    {
                    global_focus_chain.insert( i, c );
                    break;
                    }
                }
            }
        }
    updateStackingOrder();
    }

void Workspace::restoreSessionStackingOrder( Client* c )
    {
    if( c->sessionStackingOrder() < 0 )
        return;
    StackingUpdatesBlocker blocker( this );
    unconstrained_stacking_order.removeAll( c );
    ClientList::Iterator best_pos = unconstrained_stacking_order.end();
    for( ClientList::Iterator it = unconstrained_stacking_order.begin(); // from bottom
         it != unconstrained_stacking_order.end();
         ++it )
        {
        if( (*it)->sessionStackingOrder() > c->sessionStackingOrder() )
            {
            unconstrained_stacking_order.insert( it, c );
            return;
            }
        }
    unconstrained_stacking_order.append( c );
    }

void Workspace::circulateDesktopApplications()
    {
    if ( desktops.count() > 1 )
        {
        bool change_active = activeClient()->isDesktop();
        raiseClient( findDesktop( false, currentDesktop()));
        if( change_active ) // if the previously topmost Desktop was active, activate this new one
            activateClient( findDesktop( true, currentDesktop()));
        }
    // if there's no active client, make desktop the active one
    if( desktops.count() > 0 && activeClient() == NULL && should_get_focus.count() == 0 )
        activateClient( findDesktop( true, currentDesktop()));
    }


/*!
  Returns a stacking order based upon \a list that fulfills certain contained.
 */
ClientList Workspace::constrainedStackingOrder()
    {
    ClientList layer[ NumLayers ];

#if 0
    kDebug() << "stacking1:";
    for( ClientList::ConstIterator it = unconstrained_stacking_order.begin();
         it != unconstrained_stacking_order.end();
         ++it )
        kdDebug() << (void*)(*it) << *it << ":" << (*it)->layer();
#endif
    // build the order from layers
    QHash< Group*, Layer > minimum_layer;
    for( ClientList::ConstIterator it = unconstrained_stacking_order.begin();
         it != unconstrained_stacking_order.end();
         ++it )
        {
        Layer l = (*it)->layer();
        // If a window is raised above some other window in the same window group
        // which is in the ActiveLayer (i.e. it's fulscreened), make sure it stays
        // above that window (see #95731).
        if( minimum_layer.contains( (*it)->group())
            && minimum_layer[ (*it)->group() ] == ActiveLayer
            && ( l == NormalLayer || l == AboveLayer ))
            {
            l = minimum_layer[ (*it)->group() ];
            }
        minimum_layer[ (*it)->group() ] = l;
        layer[ l ].append( *it );
        }
    ClientList stacking;    
    for( Layer lay = FirstLayer;
         lay < NumLayers;
         ++lay )    
        stacking += layer[ lay ];
#if 0
    kDebug() << "stacking2:";
    for( ClientList::ConstIterator it = stacking.begin();
         it != stacking.end();
         ++it )
        kdDebug() << (void*)(*it) << *it << ":" << (*it)->layer();
#endif
    // now keep transients above their mainwindows
    // TODO this could(?) use some optimization
    for( int i = stacking.size() - 1;
         i >= 0;
         )
        {
        if( !stacking[ i ]->isTransient())
            {
            --i;
            continue;
            }
        int i2 = -1;
        if( stacking[ i ]->groupTransient())
            {
            if( stacking[ i ]->group()->members().count() > 0 )
                { // find topmost client this one is transient for
                for( i2 = stacking.size() - 1;
                     i2 >= 0;
                     --i2 )
                    {
                    if( stacking[ i2 ] == stacking[ i ] )
                        {
                        i2 = -1; // don't reorder, already the topmost in the group
                        break;
                        }
                    if( stacking[ i2 ]->hasTransient( stacking[ i ], true )
                        && keepTransientAbove( stacking[ i2 ], stacking[ i ] ))
                        break;
                    }
                } // else i2 remains pointing at -1
            }
        else
            {
            for( i2 = stacking.size() - 1;
                 i2 >= 0;
                 --i2 )
                {
                if( stacking[ i2 ] == stacking[ i ] )
                    {
                    i2 = -1; // don't reorder, already on top of its mainwindow
                    break;
                    }
                if( stacking[ i2 ] == stacking[ i ]->transientFor()
                    && keepTransientAbove( stacking[ i2 ], stacking[ i ] ))
                    break;
                }
            }
        if( i2 == -1 )
            {
            --i;
            continue;
            }
        Client* current = stacking[ i ];
        stacking.removeAt( i );
        --i; // move onto the next item (for next for() iteration)
        --i2; // adjust index of the mainwindow after the remove above
        if( !current->transients().isEmpty())  // this one now can be possibly above its transients,
            i = i2; // so go again higher in the stack order and possibly move those transients again
        ++i2; // insert after (on top of) the mainwindow, it's ok if it2 is now stacking.end()
        stacking.insert( i2, current );
        }
#if 0
    kDebug() << "stacking3:";
    for( ClientList::ConstIterator it = stacking.begin();
         it != stacking.end();
         ++it )
        kdDebug() << (void*)(*it) << *it << ":" << (*it)->layer();
    kDebug() << "\n\n";
#endif
    return stacking;
    }

void Workspace::blockStackingUpdates( bool block )
    {
    if( block )
        {
        if( block_stacking_updates == 0 )
            blocked_propagating_new_clients = false;
        ++block_stacking_updates;
        }
    else // !block
        if( --block_stacking_updates == 0 )
            updateStackingOrder( blocked_propagating_new_clients );
    }

// Ensure list is in stacking order
ClientList Workspace::ensureStackingOrder( const ClientList& list ) const
    {
// TODO    Q_ASSERT( block_stacking_updates == 0 );
    if( list.count() < 2 )
        return list;
    // TODO is this worth optimizing?
    ClientList result = list;
    for( ClientList::ConstIterator it = stacking_order.begin();
         it != stacking_order.end();
         ++it )
        if( result.removeAll( *it ) != 0 )
            result.append( *it );
    return result;
    }

// check whether a transient should be actually kept above its mainwindow
// there may be some special cases where this rule shouldn't be enfored
bool Workspace::keepTransientAbove( const Client* mainwindow, const Client* transient )
    {
    // When topmenu's mainwindow becomes active, topmenu is raised and shown.
    // They also belong to the Dock layer. This makes them to be very high.
    // Therefore don't keep group transients above them, otherwise this would move
    // group transients way too high.
    if( mainwindow->isTopMenu() && transient->groupTransient())
        return false;
    // #93832 - don't keep splashscreens above dialogs
    if( transient->isSplash() && mainwindow->isDialog())
        return false;
    // This is rather a hack for #76026. Don't keep non-modal dialogs above
    // the mainwindow, but only if they're group transient (since only such dialogs
    // have taskbar entry in Kicker). A proper way of doing this (both kwin and kicker)
    // needs to be found.
    if( transient->isDialog() && !transient->isModal() && transient->groupTransient())
        return false;
    // #63223 - don't keep transients above docks, because the dock is kept high,
    // and e.g. dialogs for them would be too high too
    if( mainwindow->isDock())
        return false;
    return true;
    }

// Returns all windows in their stacking order on the root window.
ToplevelList Workspace::xStackingOrder() const
    {
    if( !x_stacking_dirty )
        return x_stacking;
    x_stacking_dirty = false;
    x_stacking.clear();
    Window dummy;
    Window* windows = NULL;
    unsigned int count = 0;
    XQueryTree( display(), rootWindow(), &dummy, &dummy, &windows, &count );
    // use our own stacking order, not the X one, as they may differ
    foreach( Client* c, stacking_order )
        x_stacking.append( c );
    for( unsigned int i = 0;
         i < count;
         ++i )
        {
        if( Unmanaged* c = findUnmanaged( WindowMatchPredicate( windows[ i ] )))
            x_stacking.append( c );
        }
    foreach( Deleted* c, deleted )
        x_stacking.append( c );
    if( windows != NULL )
        XFree( windows );
    const_cast< Workspace* >( this )->checkUnredirect();
    return x_stacking;
    }

//*******************************
// Client
//*******************************

void Client::restackWindow( Window /*above TODO */, int detail, NET::RequestSource src, Time timestamp, bool send_event )
    {
    switch ( detail )
        {
        case Above:
        case TopIf:
            workspace()->raiseClientRequest( this, src, timestamp );
          break;
        case Below:
        case BottomIf:
            workspace()->lowerClientRequest( this, src, timestamp );
          break;
        case Opposite:
        default:
            break;
        }
    if( send_event )
        sendSyntheticConfigureNotify();
    }
    
void Client::setKeepAbove( bool b )
    {
    b = rules()->checkKeepAbove( b );
    if( b && !rules()->checkKeepBelow( false ))
        setKeepBelow( false );
    if ( b == keepAbove())
        { // force hint change if different
        if( bool( info->state() & NET::KeepAbove ) != keepAbove())
            info->setState( keepAbove() ? NET::KeepAbove : 0, NET::KeepAbove );
        return;
        }
    keep_above = b;
    info->setState( keepAbove() ? NET::KeepAbove : 0, NET::KeepAbove );
    if( decoration != NULL )
        decoration->emitKeepAboveChanged( keepAbove());
    workspace()->updateClientLayer( this );
    updateWindowRules();
    }

void Client::setKeepBelow( bool b )
    {
    b = rules()->checkKeepBelow( b );
    if( b && !rules()->checkKeepAbove( false ))
        setKeepAbove( false );
    if ( b == keepBelow())
        { // force hint change if different
        if( bool( info->state() & NET::KeepBelow ) != keepBelow())
            info->setState( keepBelow() ? NET::KeepBelow : 0, NET::KeepBelow );
        return;
        }
    keep_below = b;
    info->setState( keepBelow() ? NET::KeepBelow : 0, NET::KeepBelow );
    if( decoration != NULL )
        decoration->emitKeepBelowChanged( keepBelow());
    workspace()->updateClientLayer( this );
    updateWindowRules();
    }

Layer Client::layer() const
    {
    if( in_layer == UnknownLayer )
        const_cast< Client* >( this )->in_layer = belongsToLayer();
    return in_layer;
    }

Layer Client::belongsToLayer() const
    {
    if( isDesktop())
        return DesktopLayer;
    if( isSplash())         // no damn annoying splashscreens
        return NormalLayer; // getting in the way of everything else
    if( isDock() && keepBelow())
        // slight hack for the 'allow window to cover panel' Kicker setting
        // don't move keepbelow docks below normal window, but only to the same
        // layer, so that both may be raised to cover the other
        return NormalLayer;
    if( keepBelow())
        return BelowLayer;
    if( isDock() && !keepBelow())
        return DockLayer;
    if( isTopMenu())
        return DockLayer;
    if( isActiveFullScreen())
        return ActiveLayer;
    if( keepAbove())
        return AboveLayer;
    return NormalLayer;
    }

bool Client::isActiveFullScreen() const
    {
    // only raise fullscreen above docks if it's the topmost window in unconstrained stacking order,
    // i.e. the window set to be topmost by the user (also includes transients of the fullscreen window)
    const Client* ac = workspace()->mostRecentlyActivatedClient(); // instead of activeClient() - avoids flicker
    const Client* top = workspace()->topClientOnDesktop( workspace()->currentDesktop(), screen(), true, false );
    return( isFullScreen() && ac != NULL && top != NULL
// not needed, for xinerama  && ( ac == this || this->group() == ac->group())
        && ( top == this || this->group() == top->group()));
    }

} // namespace