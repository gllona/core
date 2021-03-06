/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef INCLUDED_SFX2_NOTEBOOKBAR_NOTEBOOKBARCONTEXTCONTROL_HXX
#define INCLUDED_SFX2_NOTEBOOKBAR_NOTEBOOKBARCONTEXTCONTROL_HXX

#include <vcl/EnumContext.hxx>

class NotebookBar;

class NotebookbarContextControl
{
public:
    virtual ~NotebookbarContextControl() {}
    virtual void SetContext( vcl::EnumContext::Context eContext ) = 0;
    virtual void SetIconClickHdl( Link<NotebookBar*, void> aHdl ) = 0;
};

#endif // INCLUDED_SFX2_NOTEBOOKBAR_NOTEBOOKBARCONTEXTCONTROL_HXX

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
