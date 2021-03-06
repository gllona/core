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

#include <wrtsh.hxx>
#include <splittbl.hxx>
#include <table.hrc>
#include <tblenum.hxx>

SwSplitTableDlg::SwSplitTableDlg( vcl::Window *pParent, SwWrtShell &rSh )
    : SvxStandardDialog( pParent, "SplitTableDialog", "modules/swriter/ui/splittable.ui" )
    , rShell(rSh)
    , m_nSplit(SplitTable_HeadlineOption::ContentCopy)
{
    get(mpContentCopyRB, "copyheading");
    get(mpBoxAttrCopyWithParaRB, "customheadingapplystyle");
    get(mpBoxAttrCopyNoParaRB, "customheading");
    get(mpBorderCopyRB, "noheading");
}

SwSplitTableDlg::~SwSplitTableDlg()
{
    disposeOnce();
}

void SwSplitTableDlg::dispose()
{
    mpContentCopyRB.clear();
    mpBoxAttrCopyWithParaRB.clear();
    mpBoxAttrCopyNoParaRB.clear();
    mpBorderCopyRB.clear();
    SvxStandardDialog::dispose();
}

void SwSplitTableDlg::Apply()
{
    m_nSplit = SplitTable_HeadlineOption::ContentCopy;
    if(mpBoxAttrCopyWithParaRB->IsChecked())
        m_nSplit = SplitTable_HeadlineOption::BoxAttrAllCopy;
    else if(mpBoxAttrCopyNoParaRB->IsChecked())
        m_nSplit = SplitTable_HeadlineOption::BoxAttrCopy;
    else if(mpBorderCopyRB->IsChecked())
        m_nSplit = SplitTable_HeadlineOption::BorderCopy;

    rShell.SplitTable(m_nSplit);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
