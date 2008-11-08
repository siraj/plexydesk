/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2006 Lubos Lunak <l.lunak@kde.org>

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

/*
 (NOTE: The compositing code is work in progress. As such this design
 documentation may get outdated in some areas.)

 The base class for compositing, implementing shared functionality
 between the OpenGL and XRender backends.
 
 Design:
 
 When compositing is turned on, XComposite extension is used to redirect
 drawing of windows to pixmaps and XDamage extension is used to get informed
 about damage (changes) to window contents. This code is mostly in composite.cpp .
 
 Workspace::performCompositing() starts one painting pass. Painting is done
 by painting the screen, which in turn paints every window. Painting can be affected
 using effects, which are chained. E.g. painting a screen means that actually
 paintScreen() of the first effect is called, which possibly does modifications
 and calls next effect's paintScreen() and so on, until Scene::finalPaintScreen()
 is called.
 
 There are 3 phases of every paint (not necessarily done together):
 The pre-paint phase, the paint phase and the post-paint phase.
 
 The pre-paint phase is used to find out about how the painting will be actually
 done (i.e. what the effects will do). For example when only a part of the screen
 needs to be updated and no effect will do any transformation it is possible to use
 an optimized paint function. How the painting will be done is controlled
 by the mask argument, see PAINT_WINDOW_* and PAINT_SCREEN_* flags in scene.h .
 For example an effect that decides to paint a normal windows as translucent
 will need to modify the mask in its prePaintWindow() to include
 the PAINT_WINDOW_TRANSLUCENT flag. The paintWindow() function will then get
 the mask with this flag turned on and will also paint using transparency.
 
 The paint pass does the actual painting, based on the information collected
 using the pre-paint pass. After running through the effects' paintScreen()
 either paintGenericScreen() or optimized paintSimpleScreen() are called.
 Those call paintWindow() on windows (not necessarily all), possibly using
 clipping to optimize performance and calling paintWindow() first with only
 PAINT_WINDOW_OPAQUE to paint the opaque parts and then later
 with PAINT_WINDOW_TRANSLUCENT to paint the transparent parts. Function
 paintWindow() again goes through effects' paintWindow() until
 finalPaintWindow() is called, which calls the window's performPaint() to
 do the actual painting.
 
 The post-paint can be used for cleanups and is also used for scheduling
 repaints during the next painting pass for animations. Effects wanting to
 repaint certain parts can manually damage them during post-paint and repaint
 of these parts will be done during the next paint pass.
 
*/

#include "scene.h"

#include <X11/extensions/shape.h>

#include "client.h"
#include "deleted.h"
#include "effects.h"

#include <qdesktopwidget.h>

namespace KWin
{

//****************************************
// Scene
//****************************************

Scene* scene;

Scene::Scene( Workspace* ws )
    : wspace( ws ),
    has_waitSync( false )
    {
    }
    
Scene::~Scene()
    {
    }

// returns mask and possibly modified region
void Scene::paintScreen( int* mask, QRegion* region )
    {
    *mask = ( *region == QRegion( 0, 0, displayWidth(), displayHeight()))
        ? 0 : PAINT_SCREEN_REGION;
    updateTimeDiff();
    // preparation step
    static_cast<EffectsHandlerImpl*>(effects)->startPaint();
    ScreenPrePaintData pdata;
    pdata.mask = *mask;
    pdata.paint = *region;
    effects->prePaintScreen( pdata, time_diff );
    *mask = pdata.mask;
    *region = pdata.paint;
    if( *mask & ( PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS ))
        { // Region painting is not possible with transformations,
          // because screen damage doesn't match transformed positions.
        *mask &= ~PAINT_SCREEN_REGION;
        *region = infiniteRegion();
        }
    else if( *mask & PAINT_SCREEN_REGION )
        { // make sure not to go outside visible screen
        *region &= QRegion( 0, 0, displayWidth(), displayHeight());
        }
    else
        { // whole screen, not transformed, force region to be full
        *region = QRegion( 0, 0, displayWidth(), displayHeight());
        }
    painted_region = *region;
    if( *mask & PAINT_SCREEN_BACKGROUND_FIRST )
        paintBackground( *region );
    ScreenPaintData data;
    effects->paintScreen( *mask, *region, data );
    foreach( Window* w, stacking_order )
        effects->postPaintWindow( effectWindow( w ));
    effects->postPaintScreen();
    *region |= painted_region;
    // make sure not to go outside of the screen area
    *region &= QRegion( 0, 0, displayWidth(), displayHeight());
    // make sure all clipping is restored
    Q_ASSERT( !PaintClipper::clip());
    }

// Compute time since the last painting pass.
void Scene::updateTimeDiff()
    {
    if( last_time.isNull())
        {
        // Painting has been idle (optimized out) for some time,
        // which means time_diff would be huge and would break animations.
        // Simply set it to one (zero would mean no change at all and could
        // cause problems).
        time_diff = 1;
        }
    else
        time_diff = last_time.elapsed();
    if( time_diff < 0 ) // check time rollback
        time_diff = 1;
    last_time.start();;
    }

// Painting pass is optimized away.
void Scene::idle()
    {
    // Don't break time since last paint for the next pass.
    last_time = QTime();
    }

// the function that'll be eventually called by paintScreen() above
void Scene::finalPaintScreen( int mask, QRegion region, ScreenPaintData& data )
    {
    if( mask & ( PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS ))
        paintGenericScreen( mask, data );
    else
        paintSimpleScreen( mask, region );
    }

// The generic painting code that can handle even transformations.
// It simply paints bottom-to-top.
void Scene::paintGenericScreen( int orig_mask, ScreenPaintData )
    {
    if( !( orig_mask & PAINT_SCREEN_BACKGROUND_FIRST ))
        paintBackground( infiniteRegion());
    QList< Phase2Data > phase2;
    foreach( Window* w, stacking_order ) // bottom to top
        {
        WindowPrePaintData data;
        data.mask = orig_mask | ( w->isOpaque() ? PAINT_WINDOW_OPAQUE : PAINT_WINDOW_TRANSLUCENT );
        w->resetPaintingEnabled();
        data.paint = infiniteRegion(); // no clipping, so doesn't really matter
        data.clip = QRegion();
        data.quads = w->buildQuads();
        // preparation step
        effects->prePaintWindow( effectWindow( w ), data, time_diff );
#ifndef NDEBUG
        foreach( const WindowQuad &q, data.quads )
            if( q.isTransformed())
                kFatal( 1212 ) << "Pre-paint calls are not allowed to transform quads!" ;
#endif
        if( !w->isPaintingEnabled())
            continue;
        phase2.append( Phase2Data( w, infiniteRegion(), data.clip, data.mask, data.quads ));
        // transformations require window pixmap
        w->suspendUnredirect( data.mask
            & ( PAINT_WINDOW_TRANSLUCENT | PAINT_SCREEN_TRANSFORMED | PAINT_WINDOW_TRANSFORMED )); 
        }

    foreach( const Phase2Data &d, phase2 )
        paintWindow( d.window, d.mask, d.region, d.quads );
    }

// The optimized case without any transformations at all.
// It can paint only the requested region and can use clipping
// to reduce painting and improve performance.
void Scene::paintSimpleScreen( int orig_mask, QRegion region )
    {
    // TODO PAINT_WINDOW_* flags don't belong here, that's why it's in the assert,
    // perhaps the two enums should be separated
    assert(( orig_mask & ( PAINT_WINDOW_TRANSFORMED | PAINT_SCREEN_TRANSFORMED
        | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS
        | PAINT_WINDOW_TRANSLUCENT | PAINT_WINDOW_OPAQUE )) == 0 );
    QHash< Window*, Phase2Data > phase2data;
    // Draw each opaque window top to bottom, subtracting the bounding rect of
    // each window from the clip region after it's been drawn.
    for( int i = stacking_order.count() - 1; // top to bottom
         i >= 0;
         --i )
        {
        Window* w = stacking_order[ i ];
        WindowPrePaintData data;
        data.mask = orig_mask | ( w->isOpaque() ? PAINT_WINDOW_OPAQUE : PAINT_WINDOW_TRANSLUCENT );
        w->resetPaintingEnabled();
        data.paint = region;
        data.clip = w->isOpaque() ? w->shape().translated( w->x(), w->y()) : QRegion();
        data.quads = w->buildQuads();
        // preparation step
        effects->prePaintWindow( effectWindow( w ), data, time_diff );
#ifndef NDEBUG
        foreach( const WindowQuad &q, data.quads )
            if( q.isTransformed())
                kFatal( 1212 ) << "Pre-paint calls are not allowed to transform quads!" ;
        if( data.mask & PAINT_WINDOW_TRANSFORMED )
            kFatal( 1212 ) << "PAINT_WINDOW_TRANSFORMED without PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS!";
#endif
        if( !w->isPaintingEnabled())
            {
            w->suspendUnredirect( true );
            continue;
            }
        if( data.paint != region ) // prepaint added area to draw
            painted_region |= data.paint; // make sure it makes it to the screen
        // Schedule the window for painting
        phase2data[w] = Phase2Data( w, data.paint, data.clip, data.mask, data.quads );
        // no transformations, but translucency requires window pixmap
        w->suspendUnredirect( data.mask & PAINT_WINDOW_TRANSLUCENT );
        }
    // Do the actual painting
    // First opaque windows, top to bottom
    // This also calculates correct paint regions for windows, also taking
    //  care of clipping
    QRegion allclips;
    for( int i = stacking_order.count() - 1; i >= 0; --i )
        {
        Window* w = stacking_order[ i ];
        if( !phase2data.contains( w ))
            continue;
        Phase2Data d = phase2data[w];
        // Calculate correct paint region and take the clip region into account
        d.region = painted_region - allclips;
        allclips |= d.clip;
        if( d.mask & PAINT_WINDOW_TRANSLUCENT )
            {
            // For translucent windows, the paint region must contain the
            //  entire painted area, except areas clipped by opaque windows
            //  above the translucent window
            phase2data[w].region = d.region;
            }
        else
            {
            // Paint the opaque window
            paintWindow( d.window, d.mask, d.region, d.quads );
            }
        }
    // Fill any areas of the root window not covered by windows
    if( !( orig_mask & PAINT_SCREEN_BACKGROUND_FIRST ))
        paintBackground( painted_region - allclips );
    // Now walk the list bottom to top, drawing translucent windows.
    for( int i = 0; i < stacking_order.count(); i++ )
        {
        Window* w = stacking_order[ i ];
        if( !phase2data.contains( w ))
            continue;
        Phase2Data d = phase2data[w];
        if( d.mask & PAINT_WINDOW_TRANSLUCENT )
            paintWindow( d.window, d.mask, d.region, d.quads );
        }
    }

void Scene::paintWindow( Window* w, int mask, QRegion region, WindowQuadList quads )
    {
    // no painting outside visible screen (and no transformations)
    region &= QRect( 0, 0, displayWidth(), displayHeight());
    if( region.isEmpty()) // completely clipped
        return;

    WindowPaintData data( w->window()->effectWindow());
    data.quads = quads;
    effects->paintWindow( effectWindow( w ), mask, region, data );
    }

// the function that'll be eventually called by paintWindow() above
void Scene::finalPaintWindow( EffectWindowImpl* w, int mask, QRegion region, WindowPaintData& data )
    {
    effects->drawWindow( w, mask, region, data );
    }

// will be eventually called from drawWindow()
void Scene::finalDrawWindow( EffectWindowImpl* w, int mask, QRegion region, WindowPaintData& data )
    {
    w->sceneWindow()->performPaint( mask, region, data );
    }

QList< QPoint > Scene::selfCheckPoints() const
    {
    QList< QPoint > ret;
    // Use QDesktopWidget directly, we're interested in "real" screens, not depending on our config.
    for( int screen = 0;
         screen < qApp->desktop()->numScreens();
         ++screen )
        { // test top-left and bottom-right of every screen
        ret.append( qApp->desktop()->screenGeometry( screen ).topLeft());
        ret.append( qApp->desktop()->screenGeometry( screen ).bottomRight() + QPoint( -5 + 1, -1 + 1 )
            + QPoint( -1, 0 )); // intentionally moved one up, since the source windows will be one down
        }
    return ret;
    }

//****************************************
// Scene::Window
//****************************************

Scene::Window::Window( Toplevel * c )
    : toplevel( c )
    , filter( ImageFilterFast )
    , disable_painting( 0 )
    , shape_valid( false )
    , cached_quad_list( NULL )
    {
    }

Scene::Window::~Window()
    {
    delete cached_quad_list;
    }

void Scene::Window::discardShape()
    {
    // it is created on-demand and cached, simply
    // reset the flag
    shape_valid = false;
    delete cached_quad_list;
    cached_quad_list = NULL;
    }

// Find out the shape of the window using the XShape extension
// or if shape is not set then simply it's the window geometry.
QRegion Scene::Window::shape() const
    {
    if( !shape_valid )
        {
        Client* c = dynamic_cast< Client* >( toplevel );
        if( toplevel->shape() || ( c != NULL && !c->mask().isEmpty()))
            {
            int count, order;
            XRectangle* rects = XShapeGetRectangles( display(), toplevel->frameId(),
                ShapeBounding, &count, &order );
            if(rects)
                {
                shape_region = QRegion();
                for( int i = 0;
                     i < count;
                     ++i )
                    shape_region += QRegion( rects[ i ].x, rects[ i ].y,
                        rects[ i ].width, rects[ i ].height );
                XFree(rects);
                // make sure the shape is sane (X is async, maybe even XShape is broken)
                shape_region &= QRegion( 0, 0, width(), height());
                }
            else
                shape_region = QRegion();
            }
        else
            shape_region = QRegion( 0, 0, width(), height());
        shape_valid = true;
        }
    return shape_region;
    }

bool Scene::Window::isVisible() const
    {
    if( dynamic_cast< Deleted* >( toplevel ) != NULL )
        return false;
    if( !toplevel->isOnCurrentDesktop())
        return false;
    if( Client* c = dynamic_cast< Client* >( toplevel ))
        return c->isShown( true );
    return true; // Unmanaged is always visible
    // TODO there may be transformations, so ignore this for now
    return !toplevel->geometry()
        .intersected( QRect( 0, 0, displayWidth(), displayHeight()))
        .isEmpty();
    }

bool Scene::Window::isOpaque() const
    {
    return toplevel->opacity() == 1.0 && !toplevel->hasAlpha();
    }

bool Scene::Window::isPaintingEnabled() const
    {
    return !disable_painting;
    }

void Scene::Window::resetPaintingEnabled()
    {
    disable_painting = 0;
    if( dynamic_cast< Deleted* >( toplevel ) != NULL )
        disable_painting |= PAINT_DISABLED_BY_DELETE;
    if( !toplevel->isOnCurrentDesktop())
        disable_painting |= PAINT_DISABLED_BY_DESKTOP;
    if( Client* c = dynamic_cast< Client* >( toplevel ))
        {
        if( c->isMinimized() )
            disable_painting |= PAINT_DISABLED_BY_MINIMIZE;
        if( c->isHiddenInternal())
            disable_painting |= PAINT_DISABLED;
        }
    }

void Scene::Window::enablePainting( int reason )
    {
    disable_painting &= ~reason;
    }

void Scene::Window::disablePainting( int reason )
    {
    disable_painting |= reason;
    }

WindowQuadList Scene::Window::buildQuads( bool force ) const
    {
    if( cached_quad_list != NULL && !force )
        return *cached_quad_list;
    WindowQuadList ret;
    if( toplevel->clientPos() == QPoint( 0, 0 ) && toplevel->clientSize() == toplevel->size())
        ret = makeQuads( WindowQuadContents, shape()); // has no decoration
    else
        {
        QRegion contents = shape() & QRect( toplevel->clientPos(), toplevel->clientSize());
        QRegion decoration = shape() - contents;
        ret = makeQuads( WindowQuadContents, contents );
        ret += makeQuads( WindowQuadDecoration, decoration );
        }
    effects->buildQuads( static_cast<Client*>( toplevel )->effectWindow(), ret );
    cached_quad_list = new WindowQuadList( ret );
    return ret;
    }

WindowQuadList Scene::Window::makeQuads( WindowQuadType type, const QRegion& reg ) const
    {
    WindowQuadList ret;
    foreach( const QRect &r, reg.rects())
        {
        WindowQuad quad( type );
        // TODO asi mam spatne pravy dolni roh - bud tady, nebo v jinych castech
        quad[ 0 ] = WindowVertex( r.x(), r.y(), r.x(), r.y());
        quad[ 1 ] = WindowVertex( r.x() + r.width(), r.y(), r.x() + r.width(), r.y());
        quad[ 2 ] = WindowVertex( r.x() + r.width(), r.y() + r.height(), r.x() + r.width(), r.y() + r.height());
        quad[ 3 ] = WindowVertex( r.x(), r.y() + r.height(), r.x(), r.y() + r.height());
        ret.append( quad );
        }
    return ret;
    }

} // namespace