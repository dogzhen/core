/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <sal/config.h>
#include <sal/log.hxx>
#include <config_folders.h>

#include <basegfx/matrix/b2dhommatrix.hxx>
#include <basegfx/matrix/b2dhommatrixtools.hxx>
#include <basegfx/polygon/b2dpolygon.hxx>
#include <basegfx/polygon/b2dpolygontools.hxx>
#include <basegfx/range/b2drectangle.hxx>
#include <osl/file.hxx>
#include <osl/process.h>
#include <rtl/bootstrap.h>
#include <rtl/strbuf.hxx>
#include <rtl/ustrbuf.hxx>
#include <tools/long.hxx>
#include <comphelper/lok.hxx>

#include <vcl/metric.hxx>
#include <vcl/fontcharmap.hxx>
#include <vcl/svapp.hxx>
#include <vcl/sysdata.hxx>

#include <quartz/ctfonts.hxx>
#include <fontsubset.hxx>
#include <impfont.hxx>
#include <impfontmetricdata.hxx>
#include <font/fontsubstitution.hxx>
#include <font/PhysicalFontCollection.hxx>

#ifdef MACOSX
#include <osx/salframe.h>
#endif
#include <quartz/utils.h>
#ifdef IOS
#include <svdata.hxx>
#endif
#include <sallayout.hxx>

#include <config_features.h>
#include <vcl/skia/SkiaHelper.hxx>
#if HAVE_FEATURE_SKIA
#include <skia/osx/gdiimpl.hxx>
#endif

using namespace vcl;

namespace {

class CoreTextGlyphFallbackSubstititution
:    public vcl::font::GlyphFallbackFontSubstitution
{
public:
    bool FindFontSubstitute(vcl::font::FontSelectPattern&, LogicalFontInstance* pLogicalFont, OUString&) const override;
};

bool FontHasCharacter(CTFontRef pFont, const OUString& rString, sal_Int32 nIndex, sal_Int32 nLen)
{
    CGGlyph glyphs[nLen];
    return CTFontGetGlyphsForCharacters(pFont, reinterpret_cast<const UniChar*>(rString.getStr() + nIndex), glyphs, nLen);
}

}

bool CoreTextGlyphFallbackSubstititution::FindFontSubstitute(vcl::font::FontSelectPattern& rPattern, LogicalFontInstance* pLogicalFont,
    OUString& rMissingChars) const
{
    bool bFound = false;
    CoreTextStyle* pStyle = static_cast<CoreTextStyle*>(pLogicalFont);
    CTFontRef pFont = static_cast<CTFontRef>(CFDictionaryGetValue(pStyle->GetStyleDict(), kCTFontAttributeName));
    CFStringRef pStr = CreateCFString(rMissingChars);
    if (pStr)
    {
        CTFontRef pFallback = CTFontCreateForString(pFont, pStr, CFRangeMake(0, CFStringGetLength(pStr)));
        if (pFallback)
        {
            bFound = true;

            // tdf#148470 remove the resolved chars from rMissing to flag which ones are still missing
            // for an attempt with another font
            OUStringBuffer aStillMissingChars;
            for (sal_Int32 nStrIndex = 0; nStrIndex < rMissingChars.getLength();)
            {
                sal_Int32 nOldStrIndex = nStrIndex;
                rMissingChars.iterateCodePoints(&nStrIndex);
                sal_Int32 nCharLength = nStrIndex - nOldStrIndex;
                if (!FontHasCharacter(pFallback, rMissingChars, nOldStrIndex, nCharLength))
                    aStillMissingChars.append(rMissingChars.getStr() + nOldStrIndex, nCharLength);
            }
            rMissingChars = aStillMissingChars.toString();

            CTFontDescriptorRef pDesc = CTFontCopyFontDescriptor(pFallback);
            FontAttributes rAttr = DevFontFromCTFontDescriptor(pDesc, nullptr);

            rPattern.maSearchName = rAttr.GetFamilyName();

            rPattern.SetWeight(rAttr.GetWeight());
            rPattern.SetItalic(rAttr.GetItalic());
            rPattern.SetPitch(rAttr.GetPitch());
            rPattern.SetWidthType(rAttr.GetWidthType());

            CFRelease(pFallback);
            CFRelease(pDesc);
        }
        CFRelease(pStr);
    }

    return bFound;
}

CoreTextFontFace::CoreTextFontFace( const FontAttributes& rDFA, sal_IntPtr nFontId )
  : vcl::font::PhysicalFontFace( rDFA )
  , mnFontId( nFontId )
{
}

CoreTextFontFace::~CoreTextFontFace()
{
}

sal_IntPtr CoreTextFontFace::GetFontId() const
{
    return mnFontId;
}

AquaSalGraphics::AquaSalGraphics()
    : mnRealDPIX( 0 )
    , mnRealDPIY( 0 )
{
    SAL_INFO( "vcl.quartz", "AquaSalGraphics::AquaSalGraphics() this=" << this );

#if HAVE_FEATURE_SKIA
    if(SkiaHelper::isVCLSkiaEnabled())
        mpBackend.reset(new AquaSkiaSalGraphicsImpl(*this, maShared));
#else
    if(false)
        ;
#endif
    else
        mpBackend.reset(new AquaGraphicsBackend(maShared));

    for (int i = 0; i < MAX_FALLBACK; ++i)
        mpTextStyle[i] = nullptr;

    if (comphelper::LibreOfficeKit::isActive())
        initWidgetDrawBackends(true);
}

AquaSalGraphics::~AquaSalGraphics()
{
    SAL_INFO( "vcl.quartz", "AquaSalGraphics::~AquaSalGraphics() this=" << this );

    maShared.unsetClipPath();

    ReleaseFonts();

    maShared.mpXorEmulation.reset();

#ifdef IOS
    if (maShared.mbForeignContext)
        return;
#endif
    if (maShared.maLayer.isSet())
    {
        CGLayerRelease(maShared.maLayer.get());
    }
    else if (maShared.maContextHolder.isSet()
#ifdef MACOSX
             && maShared.mbWindow
#endif
             )
    {
        // destroy backbuffer bitmap context that we created ourself
        CGContextRelease(maShared.maContextHolder.get());
        maShared.maContextHolder.set(nullptr);
    }
}

SalGraphicsImpl* AquaSalGraphics::GetImpl() const
{
    return mpBackend->GetImpl();
}

void AquaSalGraphics::SetTextColor( Color nColor )
{
    maShared.maTextColor = nColor;
}

void AquaSalGraphics::GetFontMetric(ImplFontMetricDataRef& rxFontMetric, int nFallbackLevel)
{
    if (nFallbackLevel < MAX_FALLBACK && mpTextStyle[nFallbackLevel])
    {
        mpTextStyle[nFallbackLevel]->GetFontMetric(rxFontMetric);
    }
}

static bool AddTempDevFont(const OUString& rFontFileURL)
{
    OUString aUSystemPath;
    OSL_VERIFY( !osl::FileBase::getSystemPathFromFileURL( rFontFileURL, aUSystemPath ) );
    OString aCFileName = OUStringToOString( aUSystemPath, RTL_TEXTENCODING_UTF8 );

    CFStringRef rFontPath = CFStringCreateWithCString(nullptr, aCFileName.getStr(), kCFStringEncodingUTF8);
    CFURLRef rFontURL = CFURLCreateWithFileSystemPath(nullptr, rFontPath, kCFURLPOSIXPathStyle, true);

    CFErrorRef error;
    bool success = CTFontManagerRegisterFontsForURL(rFontURL, kCTFontManagerScopeProcess, &error);
    if (!success)
    {
        CFRelease(error);
    }
    CFRelease(rFontPath);
    CFRelease(rFontURL);

    return success;
}

static void AddTempFontDir( const OUString &rFontDirUrl )
{
    osl::Directory aFontDir( rFontDirUrl );
    osl::FileBase::RC rcOSL = aFontDir.open();
    if( rcOSL == osl::FileBase::E_None )
    {
        osl::DirectoryItem aDirItem;

        while( aFontDir.getNextItem( aDirItem, 10 ) == osl::FileBase::E_None )
        {
            osl::FileStatus aFileStatus( osl_FileStatus_Mask_FileURL );
            rcOSL = aDirItem.getFileStatus( aFileStatus );
            if ( rcOSL == osl::FileBase::E_None )
            {
                AddTempDevFont(aFileStatus.getFileURL());
            }
        }
    }
}

static void AddLocalTempFontDirs()
{
    static bool bFirst = true;
    if( !bFirst )
        return;

    bFirst = false;

    // add private font files

    OUString aBrandStr( "$BRAND_BASE_DIR" );
    rtl_bootstrap_expandMacros( &aBrandStr.pData );

    // internal font resources, required for normal operation, like OpenSymbol
    AddTempFontDir( aBrandStr + "/" LIBO_SHARE_RESOURCE_FOLDER "/common/fonts/" );

    AddTempFontDir( aBrandStr + "/" LIBO_SHARE_FOLDER "/fonts/truetype/" );
}

void AquaSalGraphics::GetDevFontList(vcl::font::PhysicalFontCollection* pFontCollection)
{
    SAL_WARN_IF( !pFontCollection, "vcl", "AquaSalGraphics::GetDevFontList(NULL) !");

    AddLocalTempFontDirs();

    // The idea is to cache the list of system fonts once it has been generated.
    // SalData seems to be a good place for this caching. However we have to
    // carefully make the access to the font list thread-safe. If we register
    // a font-change event handler to update the font list in case fonts have
    // changed on the system we have to lock access to the list. The right
    // way to do that is the solar mutex since GetDevFontList is protected
    // through it as should be all event handlers

    SalData* pSalData = GetSalData();
    if( !pSalData->mpFontList )
        pSalData->mpFontList = GetCoretextFontList();

    // Copy all PhysicalFontFace objects contained in the SystemFontList
    pSalData->mpFontList->AnnounceFonts( *pFontCollection );

    static CoreTextGlyphFallbackSubstititution aSubstFallback;
    pFontCollection->SetFallbackHook(&aSubstFallback);
}

void AquaSalGraphics::ClearDevFontCache()
{
    SalData* pSalData = GetSalData();
    delete pSalData->mpFontList;
    pSalData->mpFontList = nullptr;
}

bool AquaSalGraphics::AddTempDevFont(vcl::font::PhysicalFontCollection*,
    const OUString& rFontFileURL, const OUString& /*rFontName*/)
{
    return ::AddTempDevFont(rFontFileURL);
}

void AquaSalGraphics::DrawTextLayout(const GenericSalLayout& rLayout)
{
    mpBackend->drawTextLayout(rLayout, rLayout.GetTextRenderModeForResolutionIndependentLayout());
}

void AquaGraphicsBackend::drawTextLayout(const GenericSalLayout& rLayout, bool bTextRenderModeForResolutionIndependentLayout)
{
#ifdef IOS
    if (!mrShared.checkContext())
    {
        SAL_WARN("vcl.quartz", "AquaSalGraphics::DrawTextLayout() without context");
        return;
    }
#endif

    const CoreTextStyle& rStyle = *static_cast<const CoreTextStyle*>(&rLayout.GetFont());
    const vcl::font::FontSelectPattern& rFontSelect = rStyle.GetFontSelectPattern();
    if (rFontSelect.mnHeight == 0)
    {
        SAL_WARN("vcl.quartz", "AquaSalGraphics::DrawTextLayout(): rFontSelect.mnHeight is zero!?");
        return;
    }

    CTFontRef pFont = static_cast<CTFontRef>(CFDictionaryGetValue(rStyle.GetStyleDict(), kCTFontAttributeName));
    CGAffineTransform aRotMatrix = CGAffineTransformMakeRotation(-rStyle.mfFontRotation);

    DevicePoint aPos;
    const GlyphItem* pGlyph;
    std::vector<CGGlyph> aGlyphIds;
    std::vector<CGPoint> aGlyphPos;
    std::vector<bool> aGlyphOrientation;
    int nStart = 0;
    while (rLayout.GetNextGlyph(&pGlyph, aPos, nStart))
    {
        CGPoint aGCPos = CGPointMake(aPos.getX(), -aPos.getY());

        // Whether the glyph should be upright in vertical mode or not
        bool bUprightGlyph = false;

        if (rStyle.mfFontRotation)
        {
            if (pGlyph->IsVertical())
            {
                bUprightGlyph = true;
                // Adjust the position of upright (vertical) glyphs.
                aGCPos.y -= CTFontGetAscent(pFont) - CTFontGetDescent(pFont);
            }
            else
            {
                // Transform the position of rotated glyphs.
                aGCPos = CGPointApplyAffineTransform(aGCPos, aRotMatrix);
            }
        }

        aGlyphIds.push_back(pGlyph->glyphId());
        aGlyphPos.push_back(aGCPos);
        aGlyphOrientation.push_back(bUprightGlyph);
    }

    if (aGlyphIds.empty())
        return;

    assert(aGlyphIds.size() == aGlyphPos.size());
#if 0
    std::cerr << "aGlyphIds:[";
    for (unsigned i = 0; i < aGlyphIds.size(); i++)
    {
        if (i > 0)
            std::cerr << ",";
        std::cerr << aGlyphIds[i];
    }
    std::cerr << "]\n";
    std::cerr << "aGlyphPos:[";
    for (unsigned i = 0; i < aGlyphPos.size(); i++)
    {
        if (i > 0)
            std::cerr << ",";
        std::cerr << aGlyphPos[i];
    }
    std::cerr << "]\n";
#endif

    mrShared.maContextHolder.saveState();
    RGBAColor textColor( mrShared.maTextColor );

    // The view is vertically flipped (no idea why), flip it back.
    CGContextScaleCTM(mrShared.maContextHolder.get(), 1.0, -1.0);
    CGContextSetShouldAntialias(mrShared.maContextHolder.get(), !mrShared.mbNonAntialiasedText);
    CGContextSetFillColor(mrShared.maContextHolder.get(), textColor.AsArray());

    if (rStyle.mbFauxBold)
    {

        float fSize = rFontSelect.mnHeight / 23.0f;
        CGContextSetStrokeColor(mrShared.maContextHolder.get(), textColor.AsArray());
        CGContextSetLineWidth(mrShared.maContextHolder.get(), fSize);
        CGContextSetTextDrawingMode(mrShared.maContextHolder.get(), kCGTextFillStroke);
    }

    if (bTextRenderModeForResolutionIndependentLayout)
    {
        CGContextSetAllowsFontSubpixelQuantization(mrShared.maContextHolder.get(), false);
        CGContextSetShouldSubpixelQuantizeFonts(mrShared.maContextHolder.get(), false);
        CGContextSetAllowsFontSubpixelPositioning(mrShared.maContextHolder.get(), true);
        CGContextSetShouldSubpixelPositionFonts(mrShared.maContextHolder.get(), true);
    }

    auto aIt = aGlyphOrientation.cbegin();
    while (aIt != aGlyphOrientation.cend())
    {
        bool bUprightGlyph = *aIt;
        // Find the boundary of the run of glyphs with the same rotation, to be
        // drawn together.
        auto aNext = std::find(aIt, aGlyphOrientation.cend(), !bUprightGlyph);
        size_t nStartIndex = std::distance(aGlyphOrientation.cbegin(), aIt);
        size_t nLen = std::distance(aIt, aNext);

        mrShared.maContextHolder.saveState();
        if (rStyle.mfFontRotation && !bUprightGlyph)
        {
            CGContextRotateCTM(mrShared.maContextHolder.get(), rStyle.mfFontRotation);
        }
        CTFontDrawGlyphs(pFont, &aGlyphIds[nStartIndex], &aGlyphPos[nStartIndex], nLen, mrShared.maContextHolder.get());
        mrShared.maContextHolder.restoreState();

        aIt = aNext;
    }

    mrShared.maContextHolder.restoreState();
}

void AquaSalGraphics::SetFont(LogicalFontInstance* pReqFont, int nFallbackLevel)
{
    // release the text style
    for (int i = nFallbackLevel; i < MAX_FALLBACK; ++i)
    {
        if (!mpTextStyle[i])
            break;
        mpTextStyle[i].clear();
    }

    if (!pReqFont)
        return;

    // update the text style
    mpTextStyle[nFallbackLevel] = static_cast<CoreTextStyle*>(pReqFont);
}

std::unique_ptr<GenericSalLayout> AquaSalGraphics::GetTextLayout(int nFallbackLevel)
{
    assert(mpTextStyle[nFallbackLevel]);
    if (!mpTextStyle[nFallbackLevel])
        return nullptr;
    return std::make_unique<GenericSalLayout>(*mpTextStyle[nFallbackLevel]);
}

FontCharMapRef AquaSalGraphics::GetFontCharMap() const
{
    if (!mpTextStyle[0])
    {
        return FontCharMapRef( new FontCharMap() );
    }

    return mpTextStyle[0]->GetFontFace()->GetFontCharMap();
}

bool AquaSalGraphics::GetFontCapabilities(vcl::FontCapabilities &rFontCapabilities) const
{
    if (!mpTextStyle[0])
        return false;

    return mpTextStyle[0]->GetFontFace()->GetFontCapabilities(rFontCapabilities);
}

// fake a SFNT font directory entry for a font table
// see http://developer.apple.com/fonts/TTRefMan/RM06/Chap6.html#Directory
static void FakeDirEntry( const char aTag[5], ByteCount nOfs, ByteCount nLen,
                          const unsigned char* /*pData*/, unsigned char*& rpDest )
{
    // write entry tag
    rpDest[ 0] = aTag[0];
    rpDest[ 1] = aTag[1];
    rpDest[ 2] = aTag[2];
    rpDest[ 3] = aTag[3];
    // TODO: get entry checksum and write it
    //      not too important since the subsetter doesn't care currently
    //      for( pData+nOfs ... pData+nOfs+nLen )
    // write entry offset
    rpDest[ 8] = static_cast<char>(nOfs >> 24);
    rpDest[ 9] = static_cast<char>(nOfs >> 16);
    rpDest[10] = static_cast<char>(nOfs >>  8);
    rpDest[11] = static_cast<char>(nOfs >>  0);
    // write entry length
    rpDest[12] = static_cast<char>(nLen >> 24);
    rpDest[13] = static_cast<char>(nLen >> 16);
    rpDest[14] = static_cast<char>(nLen >>  8);
    rpDest[15] = static_cast<char>(nLen >>  0);
    // advance to next entry
    rpDest += 16;
}

// fake a TTF or CFF font as directly accessing font file is not possible
// when only the fontid is known. This approach also handles *.font fonts.
bool AquaSalGraphics::GetRawFontData( const vcl::font::PhysicalFontFace* pFontData,
                                      std::vector<unsigned char>& rBuffer, bool* pJustCFF )
{
    const CoreTextFontFace* pMacFont = static_cast<const CoreTextFontFace*>(pFontData);

    // short circuit for CFF-only fonts
    const int nCffSize = pMacFont->GetFontTable( "CFF ", nullptr);
    if( pJustCFF != nullptr )
    {
        *pJustCFF = (nCffSize > 0);
        if( *pJustCFF)
        {
            rBuffer.resize( nCffSize);
            const int nCffRead = pMacFont->GetFontTable( "CFF ", rBuffer.data());
            if( nCffRead != nCffSize)
            {
                return false;
            }
            return true;
        }
    }

    // get font table availability and size in bytes
    const int nHeadSize = pMacFont->GetFontTable( "head", nullptr);
    if( nHeadSize <= 0)
        return false;

    const int nMaxpSize = pMacFont->GetFontTable( "maxp", nullptr);
    if( nMaxpSize <= 0)
        return false;

    const int nCmapSize = pMacFont->GetFontTable( "cmap", nullptr);
    if( nCmapSize <= 0)
        return false;

    const int nNameSize = pMacFont->GetFontTable( "name", nullptr);
    if( nNameSize <= 0)
        return false;

    const int nHheaSize = pMacFont->GetFontTable( "hhea", nullptr);
    if( nHheaSize <= 0)
        return false;

    const int nHmtxSize = pMacFont->GetFontTable( "hmtx", nullptr);
    if( nHmtxSize <= 0)
        return false;

    // get the ttf-glyf outline tables
    int nLocaSize = 0;
    int nGlyfSize = 0;
    if( nCffSize <= 0)
    {
        nLocaSize = pMacFont->GetFontTable( "loca", nullptr);
        if( nLocaSize <= 0)
            return false;

        nGlyfSize = pMacFont->GetFontTable( "glyf", nullptr);
        if( nGlyfSize <= 0)
            return false;
    }

    int nPrepSize = 0, nCvtSize = 0, nFpgmSize = 0;
    if( nGlyfSize) // TODO: reduce PDF size by making hint subsetting optional
    {
        nPrepSize = pMacFont->GetFontTable( "prep", nullptr);
        nCvtSize  = pMacFont->GetFontTable( "cvt ", nullptr);
        nFpgmSize = pMacFont->GetFontTable( "fpgm", nullptr);
    }

    // prepare a byte buffer for a fake font
    int nTableCount = 7;
    nTableCount += (nPrepSize>0?1:0) + (nCvtSize>0?1:0) + (nFpgmSize>0?1:0) + (nGlyfSize>0?1:0);
    const ByteCount nFdirSize = 12 + 16*nTableCount;
    ByteCount nTotalSize = nFdirSize;
    nTotalSize += nHeadSize + nMaxpSize + nNameSize + nCmapSize;

    if( nGlyfSize )
    {
        nTotalSize += nLocaSize + nGlyfSize;
    }
    else
    {
        nTotalSize += nCffSize;
    }
    nTotalSize += nHheaSize + nHmtxSize;
    nTotalSize += nPrepSize + nCvtSize + nFpgmSize;
    rBuffer.resize( nTotalSize );

    // fake a SFNT font directory header
    if( nTableCount < 16 )
    {
        int nLog2 = 0;
        while( (nTableCount >> nLog2) > 1 ) ++nLog2;
        rBuffer[ 1] = 1;                        // Win-TTF style scaler
        rBuffer[ 5] = nTableCount;              // table count
        rBuffer[ 7] = nLog2*16;                 // searchRange
        rBuffer[ 9] = nLog2;                    // entrySelector
        rBuffer[11] = (nTableCount-nLog2)*16;   // rangeShift
    }

    // get font table raw data and update the fake directory entries
    ByteCount nOfs = nFdirSize;
    unsigned char* pFakeEntry = &rBuffer[12];
    if( nCmapSize != pMacFont->GetFontTable( "cmap", &rBuffer[nOfs]))
        return false;

    FakeDirEntry( "cmap", nOfs, nCmapSize, rBuffer.data(), pFakeEntry );
    nOfs += nCmapSize;
    if( nCvtSize )
    {
        if( nCvtSize != pMacFont->GetFontTable( "cvt ", &rBuffer[nOfs]))
            return false;

        FakeDirEntry( "cvt ", nOfs, nCvtSize, rBuffer.data(), pFakeEntry );
        nOfs += nCvtSize;
    }
    if( nFpgmSize )
    {
        if( nFpgmSize != pMacFont->GetFontTable( "fpgm", &rBuffer[nOfs]))
            return false;

        FakeDirEntry( "fpgm", nOfs, nFpgmSize, rBuffer.data(), pFakeEntry );
        nOfs += nFpgmSize;
    }
    if( nCffSize )
    {
        if( nCffSize != pMacFont->GetFontTable( "CFF ", &rBuffer[nOfs]))
            return false;

        FakeDirEntry( "CFF ", nOfs, nCffSize, rBuffer.data(), pFakeEntry );
        nOfs += nGlyfSize;
    }
    else
    {
        if( nGlyfSize != pMacFont->GetFontTable( "glyf", &rBuffer[nOfs]))
            return false;

        FakeDirEntry( "glyf", nOfs, nGlyfSize, rBuffer.data(), pFakeEntry );
        nOfs += nGlyfSize;

        if( nLocaSize != pMacFont->GetFontTable( "loca", &rBuffer[nOfs]))
            return false;

        FakeDirEntry( "loca", nOfs, nLocaSize, rBuffer.data(), pFakeEntry );
        nOfs += nLocaSize;
    }
    if( nHeadSize != pMacFont->GetFontTable( "head", &rBuffer[nOfs]))
        return false;

    FakeDirEntry( "head", nOfs, nHeadSize, rBuffer.data(), pFakeEntry );
    nOfs += nHeadSize;

    if( nHheaSize != pMacFont->GetFontTable( "hhea", &rBuffer[nOfs]))
        return false;

    FakeDirEntry( "hhea", nOfs, nHheaSize, rBuffer.data(), pFakeEntry );
    nOfs += nHheaSize;
    if( nHmtxSize != pMacFont->GetFontTable( "hmtx", &rBuffer[nOfs]))
        return false;

    FakeDirEntry( "hmtx", nOfs, nHmtxSize, rBuffer.data(), pFakeEntry );
    nOfs += nHmtxSize;
    if( nMaxpSize != pMacFont->GetFontTable( "maxp", &rBuffer[nOfs]))
        return false;

    FakeDirEntry( "maxp", nOfs, nMaxpSize, rBuffer.data(), pFakeEntry );
    nOfs += nMaxpSize;
    if( nNameSize != pMacFont->GetFontTable( "name", &rBuffer[nOfs]))
        return false;

    FakeDirEntry( "name", nOfs, nNameSize, rBuffer.data(), pFakeEntry );
    nOfs += nNameSize;
    if( nPrepSize )
    {
        if( nPrepSize != pMacFont->GetFontTable( "prep", &rBuffer[nOfs]))
            return false;

        FakeDirEntry( "prep", nOfs, nPrepSize, rBuffer.data(), pFakeEntry );
        nOfs += nPrepSize;
    }

    SAL_WARN_IF( (nOfs!=nTotalSize), "vcl", "AquaSalGraphics::GetRawFontData (nOfs!=nTotalSize)");

    return true;
}

const void* AquaSalGraphics::GetEmbedFontData(const vcl::font::PhysicalFontFace*, tools::Long* /*pDataLen*/)
{
    return nullptr;
}

void AquaSalGraphics::FreeEmbedFontData( const void* pData, tools::Long /*nDataLen*/ )
{
    // TODO: implementing this only makes sense when the implementation of
    //      AquaSalGraphics::GetEmbedFontData() returns non-NULL
    SAL_WARN_IF( (pData==nullptr), "vcl", "AquaSalGraphics::FreeEmbedFontData() is not implemented");
}

void AquaSalGraphics::Flush()
{
    mpBackend->Flush();
}

void AquaSalGraphics::Flush( const tools::Rectangle& rRect )
{
    mpBackend->Flush( rRect );
}

#ifdef IOS

bool AquaSharedAttributes::checkContext()
{
    if (mbForeignContext)
    {
        SAL_INFO("vcl.ios", "CheckContext() this=" << this << ", mbForeignContext, return true");
        return true;
    }

    SAL_INFO( "vcl.ios", "CheckContext() this=" << this << ",  not foreign, return false");
    return false;
}

#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
