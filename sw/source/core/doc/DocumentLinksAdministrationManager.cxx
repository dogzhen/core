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

#include <DocumentLinksAdministrationManager.hxx>

#include <doc.hxx>
#include <DocumentSettingManager.hxx>
#include <IDocumentUndoRedo.hxx>
#include <IDocumentState.hxx>
#include <IDocumentMarkAccess.hxx>
#include <sfx2/objsh.hxx>
#include <sfx2/linkmgr.hxx>
#include <sfx2/docfile.hxx>
#include <dialoghelp.hxx>
#include <linkenum.hxx>
#include <com/sun/star/document/UpdateDocMode.hpp>
#include <swtypes.hxx>
#include <docsh.hxx>
#include <bookmark.hxx>
#include <swserv.hxx>
#include <swbaslnk.hxx>
#include <section.hxx>
#include <docary.hxx>
#include <frmfmt.hxx>
#include <fmtcntnt.hxx>
#include <swtable.hxx>
#include <ndtxt.hxx>
#include <frameformats.hxx>
#include <tools/urlobj.hxx>
#include <unotools/charclass.hxx>
#include <unotools/securityoptions.hxx>
#include <utility>

using namespace ::com::sun::star;

//Helper functions for this file
namespace
{
    struct FindItem
    {
        const OUString m_Item;
        SwTableNode* pTableNd;
        SwSectionNode* pSectNd;

        explicit FindItem(OUString aS)
            : m_Item(std::move(aS)), pTableNd(nullptr), pSectNd(nullptr)
        {}
     };

    ::sfx2::SvBaseLink* lcl_FindNextRemovableLink( const ::sfx2::SvBaseLinks& rLinks )
    {
        for (const auto& rLinkIter : rLinks)
        {
            ::sfx2::SvBaseLink& rLnk = *rLinkIter;
            if ((sfx2::SvBaseLinkObjectType::ClientGraphic == rLnk.GetObjType() || sfx2::SvBaseLinkObjectType::ClientFile == rLnk.GetObjType())
                && dynamic_cast<const SwBaseLink*>(&rLnk) != nullptr)
            {
                    tools::SvRef<sfx2::SvBaseLink> xLink(&rLnk);

                    OUString sFName;
                    sfx2::LinkManager::GetDisplayNames( xLink.get(), nullptr, &sFName );

                    INetURLObject aURL( sFName );
                    if( INetProtocol::File == aURL.GetProtocol() ||
                        INetProtocol::Cid == aURL.GetProtocol() )
                        return &rLnk;
            }
        }
        return nullptr;
    }


    ::sw::mark::DdeBookmark* lcl_FindDdeBookmark( const IDocumentMarkAccess& rMarkAccess, const OUString& rName, const bool bCaseSensitive )
    {
        //Iterating over all bookmarks, checking DdeBookmarks
        const OUString sNameLc = bCaseSensitive ? rName : GetAppCharClass().lowercase(rName);
        for(IDocumentMarkAccess::const_iterator_t ppMark = rMarkAccess.getAllMarksBegin();
            ppMark != rMarkAccess.getAllMarksEnd();
            ++ppMark)
        {
            if (::sw::mark::DdeBookmark* const pBkmk = dynamic_cast< ::sw::mark::DdeBookmark*>(*ppMark))
            {
                if (
                    (bCaseSensitive && (pBkmk->GetName() == sNameLc)) ||
                    (!bCaseSensitive && GetAppCharClass().lowercase(pBkmk->GetName()) == sNameLc)
                   )
                {
                    return pBkmk;
                }
            }
        }
        return nullptr;
    }


    bool lcl_FindSection( const SwSectionFormat* pSectFormat, FindItem * const pItem, bool bCaseSensitive )
    {
        SwSection* pSect = pSectFormat->GetSection();
        if( pSect )
        {
            OUString sNm( bCaseSensitive
                    ? pSect->GetSectionName()
                    : GetAppCharClass().lowercase( pSect->GetSectionName() ));
            OUString sCompare( bCaseSensitive
                    ? pItem->m_Item
                    : GetAppCharClass().lowercase( pItem->m_Item ) );
            if( sNm == sCompare )
            {
                // found, so get the data
                const SwNodeIndex* pIdx = pSectFormat->GetContent().GetContentIdx();
                if( pIdx && &pSectFormat->GetDoc()->GetNodes() == &pIdx->GetNodes() )
                {
                    // a table in the normal NodesArr
                    pItem->pSectNd = pIdx->GetNode().GetSectionNode();
                    return false;
                }
                // If the name is already correct, but not the rest then we don't have them.
                // The names are always unique.
            }
        }
        return true;
    }

    bool lcl_FindTable( const SwFrameFormat* pTableFormat, FindItem * const pItem )
    {
        OUString sNm( GetAppCharClass().lowercase( pTableFormat->GetName() ));
        if ( sNm == pItem->m_Item )
        {
            SwTable* pTmpTable = SwTable::FindTable( pTableFormat );
            if( pTmpTable )
            {
                SwTableBox* pFBox = pTmpTable->GetTabSortBoxes()[0];
                if( pFBox && pFBox->GetSttNd() &&
                    &pTableFormat->GetDoc()->GetNodes() == &pFBox->GetSttNd()->GetNodes() )
                {
                    // a table in the normal NodesArr
                    pItem->pTableNd = const_cast<SwTableNode*>(
                                                pFBox->GetSttNd()->FindTableNode());
                    return false;
                }
            }
            // If the name is already correct, but not the rest then we don't have them.
            // The names are always unique.
        }
        return true;
    }

}


namespace sw
{

DocumentLinksAdministrationManager::DocumentLinksAdministrationManager( SwDoc& i_rSwdoc )
    : mbVisibleLinks(true)
    , mbLinksUpdated( false ) //#i38810#
    , m_pLinkMgr( new sfx2::LinkManager(nullptr) )
    , m_rDoc( i_rSwdoc )
{
}

bool DocumentLinksAdministrationManager::IsVisibleLinks() const
{
    return mbVisibleLinks;
}

void DocumentLinksAdministrationManager::SetVisibleLinks(bool bFlag)
{
    mbVisibleLinks = bFlag;
}

sfx2::LinkManager& DocumentLinksAdministrationManager::GetLinkManager()
{
    return *m_pLinkMgr;
}

const sfx2::LinkManager& DocumentLinksAdministrationManager::GetLinkManager() const
{
    return *m_pLinkMgr;
}

// #i42634# Moved common code of SwReader::Read() and SwDocShell::UpdateLinks()
// to new SwDoc::UpdateLinks():
void DocumentLinksAdministrationManager::UpdateLinks()
{
    if (!m_rDoc.GetDocShell())
        return;
    SfxObjectCreateMode eMode = m_rDoc.GetDocShell()->GetCreateMode();
    if (eMode == SfxObjectCreateMode::INTERNAL)
        return;
    if (eMode == SfxObjectCreateMode::ORGANIZER)
        return;
    if (m_rDoc.GetDocShell()->IsPreview())
        return;
    if (GetLinkManager().GetLinks().empty())
        return;
    sal_uInt16 nLinkMode = m_rDoc.GetDocumentSettingManager().getLinkUpdateMode(true);
    sal_uInt16 nUpdateDocMode = m_rDoc.GetDocShell()->GetUpdateDocMode();
    if (nLinkMode == NEVER && nUpdateDocMode != document::UpdateDocMode::FULL_UPDATE)
        return;

    bool bAskUpdate = nLinkMode == MANUAL;
    bool bUpdate = true;
    switch(nUpdateDocMode)
    {
        case document::UpdateDocMode::NO_UPDATE:   bUpdate = false;break;
        case document::UpdateDocMode::QUIET_UPDATE:bAskUpdate = false; break;
        case document::UpdateDocMode::FULL_UPDATE: bAskUpdate = true; break;
    }
    if (nLinkMode == AUTOMATIC && !bAskUpdate)
    {
        SfxMedium * medium = m_rDoc.GetDocShell()->GetMedium();
        if (!SvtSecurityOptions::isTrustedLocationUriForUpdatingLinks(
                medium == nullptr ? OUString() : medium->GetName()))
        {
            bAskUpdate = true;
        }
    }
    comphelper::EmbeddedObjectContainer& rEmbeddedObjectContainer = m_rDoc.GetDocShell()->getEmbeddedObjectContainer();
    if (bUpdate)
    {
        rEmbeddedObjectContainer.setUserAllowsLinkUpdate(true);

        weld::Window* pDlgParent = GetFrameWeld(m_rDoc.GetDocShell());
        GetLinkManager().UpdateAllLinks(bAskUpdate, false, pDlgParent);
    }
    else
    {
        rEmbeddedObjectContainer.setUserAllowsLinkUpdate(false);
    }
}

bool DocumentLinksAdministrationManager::GetData( const OUString& rItem, const OUString& rMimeType,
                     uno::Any & rValue ) const
{
    // search for bookmarks and sections case sensitive at first. If nothing is found then try again case insensitive
    bool bCaseSensitive = true;
    while( true )
    {
        ::sw::mark::DdeBookmark* const pBkmk = lcl_FindDdeBookmark(*m_rDoc.getIDocumentMarkAccess(), rItem, bCaseSensitive);
        if(pBkmk)
            return SwServerObject(*pBkmk).GetData(rValue, rMimeType);

        // Do we already have the Item?
        OUString sItem( bCaseSensitive ? rItem : GetAppCharClass().lowercase(rItem));
        FindItem aPara( sItem );
        for( const SwSectionFormat* pFormat : m_rDoc.GetSections() )
        {
            if (!(lcl_FindSection(pFormat, &aPara, bCaseSensitive)))
                break;
        }
        if( aPara.pSectNd )
        {
            // found, so get the data
            return SwServerObject( *aPara.pSectNd ).GetData( rValue, rMimeType );
        }
        if( !bCaseSensitive )
            break;
        bCaseSensitive = false;
    }

    FindItem aPara( GetAppCharClass().lowercase( rItem ));
    for( const SwFrameFormat* pFormat : *m_rDoc.GetTableFrameFormats() )
    {
        if (!(lcl_FindTable(pFormat, &aPara)))
            break;
    }
    if( aPara.pTableNd )
    {
        return SwServerObject( *aPara.pTableNd ).GetData( rValue, rMimeType );
    }

    return false;
}

void DocumentLinksAdministrationManager::SetData( const OUString& rItem )
{
    // search for bookmarks and sections case sensitive at first. If nothing is found then try again case insensitive
    bool bCaseSensitive = true;
    while( true )
    {
        ::sw::mark::DdeBookmark* const pBkmk = lcl_FindDdeBookmark(*m_rDoc.getIDocumentMarkAccess(), rItem, bCaseSensitive);
        if(pBkmk)
        {
            return;
        }

        // Do we already have the Item?
        OUString sItem( bCaseSensitive ? rItem : GetAppCharClass().lowercase(rItem));
        FindItem aPara( sItem );
        for( const SwSectionFormat* pFormat : m_rDoc.GetSections() )
        {
            if (!(lcl_FindSection(pFormat, &aPara, bCaseSensitive)))
                break;
        }
        if( aPara.pSectNd )
        {
            // found, so get the data
            return;
        }
        if( !bCaseSensitive )
            break;
        bCaseSensitive = false;
    }

    OUString sItem(GetAppCharClass().lowercase(rItem));
    FindItem aPara( sItem );
    for( const SwFrameFormat* pFormat : *m_rDoc.GetTableFrameFormats() )
    {
        if (!(lcl_FindTable(pFormat, &aPara)))
            break;
    }
}

::sfx2::SvLinkSource* DocumentLinksAdministrationManager::CreateLinkSource(const OUString& rItem)
{
    SwServerObject* pObj = nullptr;

    // search for bookmarks and sections case sensitive at first. If nothing is found then try again case insensitive
    bool bCaseSensitive = true;
    while( true )
    {
        // bookmarks
        ::sw::mark::DdeBookmark* const pBkmk = lcl_FindDdeBookmark(*m_rDoc.getIDocumentMarkAccess(), rItem, bCaseSensitive);
        if(pBkmk && pBkmk->IsExpanded())
        {
            pObj = pBkmk->GetRefObject();
            if( !pObj )
            {
                // mark found, but no link yet -> create hotlink
                pObj = new SwServerObject(*pBkmk);
                pBkmk->SetRefObject(pObj);
                GetLinkManager().InsertServer(pObj);
            }
        }
        if(pObj)
            return pObj;

        FindItem aPara(bCaseSensitive ? rItem : GetAppCharClass().lowercase(rItem));
        // sections
        for( const SwSectionFormat* pFormat : m_rDoc.GetSections() )
        {
            if (!(lcl_FindSection(pFormat, &aPara, bCaseSensitive)))
                break;
        }

        if(aPara.pSectNd)
        {
            pObj = aPara.pSectNd->GetSection().GetObject();
            if( !pObj )
            {
                // section found, but no link yet -> create hotlink
                pObj = new SwServerObject( *aPara.pSectNd );
                aPara.pSectNd->GetSection().SetRefObject( pObj );
                GetLinkManager().InsertServer(pObj);
            }
        }
        if(pObj)
            return pObj;
        if( !bCaseSensitive )
            break;
        bCaseSensitive = false;
    }

    FindItem aPara( GetAppCharClass().lowercase(rItem) );
    // tables
    for( const SwFrameFormat* pFormat : *m_rDoc.GetTableFrameFormats() )
    {
        if (!(lcl_FindTable(pFormat, &aPara)))
            break;
    }
    if(aPara.pTableNd)
    {
        pObj = aPara.pTableNd->GetTable().GetObject();
        if( !pObj )
        {
            // table found, but no link yet -> create hotlink
            pObj = new SwServerObject(*aPara.pTableNd);
            aPara.pTableNd->GetTable().SetRefObject(pObj);
            GetLinkManager().InsertServer(pObj);
        }
    }
    return pObj;
}

/// embedded all local links (Areas/Graphics)
bool DocumentLinksAdministrationManager::EmbedAllLinks()
{
    bool bRet = false;
    sfx2::LinkManager& rLnkMgr = GetLinkManager();
    const ::sfx2::SvBaseLinks& rLinks = rLnkMgr.GetLinks();
    if( !rLinks.empty() )
    {
        ::sw::UndoGuard const undoGuard(m_rDoc.GetIDocumentUndoRedo());

        ::sfx2::SvBaseLink* pLnk = nullptr;
        while( nullptr != (pLnk = lcl_FindNextRemovableLink( rLinks ) ) )
        {
            tools::SvRef<sfx2::SvBaseLink> xLink = pLnk;
            // Tell the link that it's being destroyed!
            xLink->Closed();

            // if one forgot to remove itself
            if( xLink.is() )
                rLnkMgr.Remove( xLink.get() );

            bRet = true;
        }

        m_rDoc.GetIDocumentUndoRedo().DelAllUndoObj();
        m_rDoc.getIDocumentState().SetModified();
    }
    return bRet;
}

void DocumentLinksAdministrationManager::SetLinksUpdated(const bool bNewLinksUpdated)
{
    mbLinksUpdated = bNewLinksUpdated;
}

bool DocumentLinksAdministrationManager::LinksUpdated() const
{
    return mbLinksUpdated;
}

DocumentLinksAdministrationManager::~DocumentLinksAdministrationManager()
{
}

bool DocumentLinksAdministrationManager::SelectServerObj( std::u16string_view rStr, SwPaM*& rpPam, std::optional<SwNodeRange>& roRange ) const
{
    // Do we actually have the Item?
    rpPam = nullptr;
    roRange.reset();

    OUString sItem( INetURLObject::decode( rStr,
                                         INetURLObject::DecodeMechanism::WithCharset ));

    sal_Int32 nPos = sItem.indexOf( cMarkSeparator );

    const CharClass& rCC = GetAppCharClass();

    // Extension for sections: not only link bookmarks/sections
    // but also frames (text!), tables, outlines:
    if( -1 != nPos )
    {
        bool bContinue = false;
        OUString sName( sItem.copy( 0, nPos ) );
        std::u16string_view sCmp( sItem.subView( nPos + 1 ));
        sItem = rCC.lowercase( sItem );

        FindItem aPara( sName );

        if( sCmp == u"table" )
        {
            sName = rCC.lowercase( sName );
            for( const SwFrameFormat* pFormat : *m_rDoc.GetTableFrameFormats() )
            {
                if (!(lcl_FindTable(pFormat, &aPara)))
                    break;
            }
            if( aPara.pTableNd )
            {
                roRange.emplace( *aPara.pTableNd, SwNodeOffset(0),
                                 *aPara.pTableNd->EndOfSectionNode(), SwNodeOffset(1) );
                return true;
            }
        }
        else if( sCmp == u"frame" )
        {
            const SwFlyFrameFormat* pFlyFormat = m_rDoc.FindFlyByName( sName );
            if( pFlyFormat )
            {
                SwNodeIndex* pIdx = const_cast<SwNodeIndex*>(pFlyFormat->GetContent().GetContentIdx());
                if( pIdx )
                {
                    SwNode* pNd = &pIdx->GetNode();
                    if( !pNd->IsNoTextNode() )
                    {
                        roRange.emplace( *pNd, SwNodeOffset(1), *pNd->EndOfSectionNode() );
                        return true;
                    }
                }
            }
        }
        else if( sCmp == u"region" )
        {
            sItem = sName;              // Is being dealt with further down!
            bContinue = true;
        }
        else if( sCmp == u"outline" )
        {
            SwPosition aPos( m_rDoc.GetNodes() );
            if (m_rDoc.GotoOutline(aPos, sName, nullptr))
            {
                SwNode* pNd = &aPos.GetNode();
                const int nLvl = pNd->GetTextNode()->GetAttrOutlineLevel()-1;

                const SwOutlineNodes& rOutlNds = m_rDoc.GetNodes().GetOutLineNds();
                SwOutlineNodes::size_type nTmpPos;
                (void)rOutlNds.Seek_Entry( pNd, &nTmpPos );
                roRange.emplace( aPos.nNode, SwNodeOffset(0), aPos.nNode );

                // look for the section's end, now
                for( ++nTmpPos;
                        nTmpPos < rOutlNds.size() &&
                        nLvl < rOutlNds[ nTmpPos ]->GetTextNode()->
                                GetAttrOutlineLevel()-1;
                    ++nTmpPos )
                    ;       // there is no block

                if( nTmpPos < rOutlNds.size() )
                    roRange->aEnd = *rOutlNds[ nTmpPos ];
                else
                    roRange->aEnd = m_rDoc.GetNodes().GetEndOfContent();
                return true;
            }
        }

        if( !bContinue )
            return false;
    }

    // search for bookmarks and sections case sensitive at first. If nothing is found then try again case insensitive
    bool bCaseSensitive = true;
    while( true )
    {
        ::sw::mark::DdeBookmark* const pBkmk = lcl_FindDdeBookmark(*m_rDoc.getIDocumentMarkAccess(), sItem, bCaseSensitive);
        if(pBkmk)
        {
            if(pBkmk->IsExpanded())
                rpPam = new SwPaM(
                    pBkmk->GetMarkPos(),
                    pBkmk->GetOtherMarkPos());
            return static_cast<bool>(rpPam);
        }

        FindItem aPara( bCaseSensitive ? sItem : rCC.lowercase( sItem ) );

        if( !m_rDoc.GetSections().empty() )
        {
            for( const SwSectionFormat* pFormat : m_rDoc.GetSections() )
            {
                if (!(lcl_FindSection(pFormat, &aPara, bCaseSensitive)))
                    break;
            }
            if( aPara.pSectNd )
            {
                roRange.emplace( *aPara.pSectNd, SwNodeOffset(1),
                                 *aPara.pSectNd->EndOfSectionNode() );
                return true;

            }
        }
        if( !bCaseSensitive )
            break;
        bCaseSensitive = false;
    }
    return false;
}


}


/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
