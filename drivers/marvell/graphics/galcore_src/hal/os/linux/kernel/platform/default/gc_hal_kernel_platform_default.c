/****************************************************************************
*
*    Copyright (c) 2005 - 2015 by Vivante Corp.
*    
*    This program is free software; you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation; either version 2 of the license, or
*    (at your option) any later version.
*    
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*    GNU General Public License for more details.
*    
*    You should have received a copy of the GNU General Public License
*    along with this program; if not write to the Free Software
*    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*
*****************************************************************************/



#include "gc_hal_kernel_linux.h"
#include "gc_hal_kernel_platform.h"

gctBOOL
_NeedAddDevice(
    IN gckPLATFORM Platform
    )
{
#if MRVL_USE_GPU_RESERVE_MEM
    return gcvFALSE;
#else
    return gcvTRUE;
#endif
}

gcsPLATFORM_OPERATIONS platformOperations =
{
    .needAddDevice = _NeedAddDevice,
};

void
gckPLATFORM_QueryOperations(
    IN gcsPLATFORM_OPERATIONS ** Operations
    )
{
     *Operations = &platformOperations;
}
