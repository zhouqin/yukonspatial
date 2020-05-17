/******************************************************************************
 * $Id: ogrsuadriver.cpp 27044 2014-03-16 23:41:27Z rouault $
 *
 * Project:  SUA Translator
 * Purpose:  Implements OGRSUADriver.
 * Author:   Even Rouault, even dot rouault at mines dash paris dot org
 *
 ******************************************************************************
 * Copyright (c) 2010, Even Rouault <even dot rouault at mines-paris dot org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "ogr_sua.h"
#include "cpl_conv.h"

CPL_CVSID("$Id: ogrsuadriver.cpp 27044 2014-03-16 23:41:27Z rouault $");

extern "C" void RegisterOGRSUA();

/************************************************************************/
/*                           ~OGRSUADriver()                            */
/************************************************************************/

OGRSUADriver::~OGRSUADriver()

{
}

/************************************************************************/
/*                              GetName()                               */
/************************************************************************/

const char *OGRSUADriver::GetName()

{
    return "SUA";
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

OGRDataSource *OGRSUADriver::Open( const char * pszFilename, int bUpdate )

{
    OGRSUADataSource   *poDS = new OGRSUADataSource();

    if( !poDS->Open( pszFilename, bUpdate ) )
    {
        delete poDS;
        poDS = NULL;
    }

    return poDS;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRSUADriver::TestCapability( const char * pszCap )

{
    return FALSE;
}

/************************************************************************/
/*                           RegisterOGRSUA()                           */
/************************************************************************/

void RegisterOGRSUA()

{
    OGRSFDriverRegistrar::GetRegistrar()->RegisterDriver( new OGRSUADriver );
}

