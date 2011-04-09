//
//  Copyright (C) 2010, 2011 Alexey Bobkov
//
//  This file is part of Fb2toepub converter.
//
//  Fb2toepub converter is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  Fb2toepub converter is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with Fb2toepub converter.  If not, see <http://www.gnu.org/licenses/>.
//


#include "hdr.h"

namespace Fb2ToEpub
{

//-----------------------------------------------------------------------
// test mode stuff (to make output file predictable for automatic testing)
static unsigned int testModeFlags = 0;
void FB2TOEPUB_DECL SetTestMode(unsigned int flags) {testModeFlags = flags;}
unsigned int FB2TOEPUB_DECL IsTestMode()            {return testModeFlags;}


};  //namespace Fb2ToEpub