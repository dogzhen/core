/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef INCLUDED_SFX2_LOKCHARTHELPER_HXX
#define INCLUDED_SFX2_LOKCHARTHELPER_HXX

#include <sfx2/dllapi.h>
#include <tools/gen.hxx>
#include <vcl/vclptr.hxx>
#include <vcl/window.hxx>

namespace com::sun::star::beans { struct PropertyValue; }
namespace com::sun::star::frame { class XController; }
namespace com::sun::star::frame { class XDispatch; }
namespace com::sun::star::uno { template<class E> class Sequence; }

class SfxViewShell;
class VirtualDevice;

/// A class for chart editing support via LibreOfficeKit.
class SFX2_DLLPUBLIC LokChartHelper
{
private:
    SfxViewShell* mpViewShell;
    css::uno::Reference<css::frame::XController> mxController;
    css::uno::Reference<css::frame::XDispatch> mxDispatcher;
    VclPtr<vcl::Window> mpWindow;
    bool mbNegativeX;

public:
    LokChartHelper(SfxViewShell* pViewShell, bool bNegativeX = false)
        : mpViewShell(pViewShell)
        , mpWindow(nullptr)
        , mbNegativeX(bNegativeX)
    {}

    css::uno::Reference<css::frame::XController>& GetXController();
    css::uno::Reference<css::frame::XDispatch>& GetXDispatcher();
    vcl::Window* GetWindow();
    tools::Rectangle GetChartBoundingBox();
    void Invalidate();

    bool Hit(const Point& aPos);
    static bool HitAny(const Point& aPos, bool bNegativeX = false);
    void PaintTile(VirtualDevice& rRenderContext, const tools::Rectangle& rTileRect);
    static void PaintAllChartsOnTile(VirtualDevice& rDevice,
                                     int nOutputWidth, int nOutputHeight,
                                     int nTilePosX, int nTilePosY,
                                     tools::Long nTileWidth, tools::Long nTileHeight,
                                     bool bNegativeX = false);
    bool postMouseEvent(int nType, int nX, int nY,
                        int nCount, int nButtons, int nModifier,
                        double fScaleX = 1.0, double fScaleY = 1.0);
    bool setTextSelection(int nType, int nX, int nY);
    bool setGraphicSelection(int nType, int nX, int nY,
                             double fScaleX = 1.0, double fScaleY = 1.0);
};

/// A class for math editing support via LibreOfficeKit.
class SFX2_DLLPUBLIC LokStarMathHelper
{
public:
    LokStarMathHelper(SfxViewShell* pViewShell)
        : mpViewShell(pViewShell)
    {
    }

    vcl::Window* GetGraphicWindow();
    vcl::Window* GetWidgetWindow();

    void Dispatch(const OUString& cmd, const css::uno::Sequence<css::beans::PropertyValue>& rArguments);

    bool postMouseEvent(int nType, int nX, int nY, int nCount, int nButtons, int nModifier,
                        double fScaleX = 1.0, double fScaleY = 1.0);

private:
    css::uno::Reference<css::frame::XController>& GetXController();
    tools::Rectangle GetBoundingBox();

    SfxViewShell* mpViewShell;
    css::uno::Reference<css::frame::XController> mxController;
    VclPtr<vcl::Window> mpGraphicWindow;
    VclPtr<vcl::Window> mpWidgetWindow;
};

#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
