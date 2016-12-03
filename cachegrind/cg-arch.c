/*--------------------------------------------------------------------*/
/*--- Cachegrind: cache configuration.                             ---*/
/*--- The architecture specific void VG_(configure_caches) are     ---*/
/*--- located in the cg-<architecture>.c files.                    ---*/
/*---                                                    cg-arch.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Cachegrind, a Valgrind tool for cache
   profiling programs.

   Copyright (C) 2011-2012 Nicholas Nethercote
      njn@valgrind.org

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#include "pub_tool_basics.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_options.h"

#include "cg_arch.h"

Bool TLB_bool=False, itlb_bool=False, dtlb_bool=False, l2tlb_bool=False;

// Checks cache config is ok.  Returns NULL if ok, or a pointer to an error
// string otherwise.
static Char* check_cache(cache_t* cache)
{
   // Simulator requires set count to be a power of two.
   if ((cache->size % (cache->line_size * cache->assoc) != 0) ||
       (-1 == VG_(log2)(cache->size/cache->line_size/cache->assoc)))
   {
      return "Cache set count is not a power of two.\n";
   }

   // Simulator requires line size to be a power of two.
   if (-1 == VG_(log2)(cache->line_size)) {
      return "Cache line size is not a power of two.\n";
   }

   // Then check line size >= 16 -- any smaller and a single instruction could
   // straddle three cache lines, which breaks a simulation assertion and is
   // stupid anyway.
   if (cache->line_size < MIN_LINE_SIZE) {
      return "Cache line size is too small.\n";
   }

   /* Then check cache size > line size (causes seg faults if not). */
   if (cache->size <= cache->line_size) {
      return "Cache size <= line size.\n";
   }

   /* Then check assoc <= (size / line size) (seg faults otherwise). */
   if (cache->assoc > (cache->size / cache->line_size)) {
      return "Cache associativity > (size / line size).\n";
   }

   return NULL;
}


static void parse_cache_opt ( cache_t* cache, Char* opt, Char* optval )
{
   Long i1, i2, i3;
   Char* endptr;
   Char* checkRes;

   // Option argument looks like "65536,2,64".  Extract them.
   i1 = VG_(strtoll10)(optval,   &endptr); if (*endptr != ',')  goto bad;
   i2 = VG_(strtoll10)(endptr+1, &endptr); if (*endptr != ',')  goto bad;
   i3 = VG_(strtoll10)(endptr+1, &endptr); if (*endptr != '\0') goto bad;

   // Check for overflow.
   cache->size      = (Int)i1;
   cache->assoc     = (Int)i2;
   cache->line_size = (Int)i3;
   if (cache->size      != i1) goto overflow;
   if (cache->assoc     != i2) goto overflow;
   if (cache->line_size != i3) goto overflow;

    if(!TLB_bool){
        checkRes = check_cache(cache);
        if (checkRes) {
            VG_(fmsg)("%s", checkRes);
            goto bad;
        }
    }

   return;

  bad:
   VG_(fmsg_bad_option)(opt, "");

  overflow:
   VG_(fmsg_bad_option)(opt,
      "One of the cache parameters was too large and overflowed.\n");
}


Bool VG_(str_clo_cache_opt)(Char *arg,
                            cache_t* clo_I1c,
                            cache_t* clo_D1c,
                            cache_t* clo_L2c,
                            cache_t* clo_LLc,
                            cache_t* clo_iTLBc,
                            cache_t* clo_dTLBc,
                            cache_t* clo_L2TLBc)
{
   Char* tmp_str;

   if      VG_STR_CLO(arg, "--I1", tmp_str) {
      parse_cache_opt(clo_I1c, arg, tmp_str);
      return True;
   } else if VG_STR_CLO(arg, "--D1", tmp_str) {
      parse_cache_opt(clo_D1c, arg, tmp_str);
      return True;
   } else if (VG_STR_CLO(arg,"--L2", tmp_str)){
      parse_cache_opt(clo_L2c, arg, tmp_str);
      return True;
   }else if (VG_STR_CLO(arg, "--LL", tmp_str)) {
      parse_cache_opt(clo_LLc, arg, tmp_str);
      return True;
   } else if (VG_STR_CLO(arg,"--iTLB", tmp_str)) {
       TLB_bool=True;
       parse_cache_opt(clo_iTLBc, arg, tmp_str);
       itlb_bool=True;
       return True;
   } else if (VG_STR_CLO(arg,"--dTLB", tmp_str)) {
       TLB_bool=True;
       parse_cache_opt(clo_dTLBc, arg, tmp_str);
       dtlb_bool=True;
       return True;
   } else if (VG_STR_CLO(arg,"--L2TLB", tmp_str)) {
       TLB_bool=True;
       parse_cache_opt(clo_L2TLBc, arg, tmp_str);
       l2tlb_bool=True;
       return True;
   }else
      return False;
}

static void umsg_cache_img(Char* desc, cache_t* c)
{
   VG_(umsg)("  %s: %'d B, %d-way, %d B lines\n", desc,
             c->size, c->assoc, c->line_size);
}

// Verifies if c is a valid cache.
// An invalid value causes an assert, unless clo_redefined is True.
static void check_cache_or_override(Char* desc, cache_t* c, Bool clo_redefined)
{
   Char* checkRes;

   checkRes = check_cache(c);
   if (checkRes) {
      VG_(umsg)("Auto-detected %s cache configuration not supported: %s",
                desc, checkRes);
      umsg_cache_img(desc, c);
      if (!clo_redefined) {
         VG_(umsg)("As it probably should be supported, please report a bug!\n");
         VG_(umsg)("Bypass this message by using option --%s=...\n", desc);
         tl_assert(0);
      }
   }
}

void VG_(post_clo_init_configure_caches)(cache_t* I1c,
                                         cache_t* D1c,
                                         cache_t *L2c,
                                         cache_t* LLc,
                                         cache_t* iTLBc,
                                         cache_t* dTLBc,
                                         cache_t* L2TLBc,
                                         cache_t* clo_I1c,
                                         cache_t* clo_D1c,
                                         cache_t* clo_L2c,
                                         cache_t* clo_LLc,
                                         cache_t* clo_iTLBc,
                                         cache_t* clo_dTLBc,
                                         cache_t* clo_L2TLBc)
{
#define DEFINED(L)   (-1 != L->size  || -1 != L->assoc || -1 != L->line_size)
   // Count how many were defined on the command line.
   Bool all_caches_clo_defined =
      (DEFINED(clo_I1c)  &&
       DEFINED(clo_D1c)  &&
	   DEFINED(clo_L2c)  &&
       DEFINED(clo_LLc)  );
       //DEFINED(clo_iTLBc)&&
       //DEFINED(clo_dTLBc)&&
       //DEFINED(clo_L2TLBc) );

   // Set the cache config (using auto-detection, if supported by the
   // architecture).
   VG_(configure_caches)( I1c, D1c,L2c, LLc, iTLBc, dTLBc, L2TLBc, all_caches_clo_defined );

   // Check the default/auto-detected values.
   // Allow the user to override invalid auto-detected caches
   // with command line.
   check_cache_or_override ("I1", I1c, DEFINED(clo_I1c));
   check_cache_or_override ("D1", D1c, DEFINED(clo_D1c));
   check_cache_or_override ("L2", L2c, DEFINED(clo_L2c));
   check_cache_or_override ("LL", LLc, DEFINED(clo_LLc));
    
   //don't check tlb here, because it is not line with the CPU cache constraints
   //check_cache_or_override ("iTLB", iTLBc, DEFINED(clo_iTLBc));
   //check_cache_or_override ("dTLB", dTLBc, DEFINED(clo_dTLBc));
   //check_cache_or_override ("L2TLB", L2TLBc, DEFINED(clo_L2TLBc));
    
   // Then replace with any defined on the command line.  (Already checked in
   // VG(parse_clo_cache_opt)().)
   if (DEFINED(clo_I1c)) { *I1c = *clo_I1c; }
   if (DEFINED(clo_D1c)) { *D1c = *clo_D1c; }
   if (DEFINED(clo_L2c)) { *L2c = *clo_L2c; }
   if (DEFINED(clo_LLc)) { *LLc = *clo_LLc; }
   if (itlb_bool) { *iTLBc = *clo_iTLBc;   }
   if (dtlb_bool) { *dTLBc = *clo_dTLBc;   }
   if (l2tlb_bool){ *L2TLBc = *clo_L2TLBc; }
    
   if (VG_(clo_verbosity) >= 2) {
      VG_(umsg)("Cache configuration used:\n");
      umsg_cache_img ("I1", I1c);
      umsg_cache_img ("D1", D1c);
      umsg_cache_img ("L2", L2c);
      umsg_cache_img ("LL", LLc);
      //don't print TLB here
      //umsg_cache_img ("iTLB", iTLBc);
      //umsg_cache_img ("dTLB", dTLBc);
      //umsg_cache_img ("L2TLB", L2TLBc);
   }
#undef DEFINED
}

void VG_(print_cache_clo_opts)()
{
   VG_(printf)(
"    --I1=<size>,<assoc>,<line_size>  set I1 cache manually\n"
"    --D1=<size>,<assoc>,<line_size>  set D1 cache manually\n"
"    --L2=<size>,<assoc>,<line_size>  set L2 cache manually\n"
"    --LL=<size>,<assoc>,<line_size>  set LL cache manually\n"
"    --iTLB=<page_size>,<assoc>,<entries>  set iTLB cache manually\n"
"    --dTLB=<page_size>,<assoc>,<entries>  set dTLB cache manually\n"
"    --L2TLB=<page_size>,<assoc>,<entries> set L2TLB cache manually\n"
               );
}
