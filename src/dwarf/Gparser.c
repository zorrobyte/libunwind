/* libunwind - a platform-independent unwind library
   Copyright (c) 2003, 2005 Hewlett-Packard Development Company, L.P.
        Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#include "dwarf_i.h"
#include "libunwind_i.h"
#include <stddef.h>
#include <limits.h>

#define alloc_reg_state()       (mempool_alloc (&dwarf_reg_state_pool))
#define free_reg_state(rs)      (mempool_free (&dwarf_reg_state_pool, rs))

#define DWARF_UNW_CACHE_SIZE(log_size)   (1 << log_size)
#define DWARF_UNW_HASH_SIZE(log_size)    (1 << (log_size + 1))

static inline int
read_regnum (unw_addr_space_t as, unw_accessors_t *a, unw_word_t *addr,
             unw_word_t *valp, void *arg)
{
  int ret;

  if ((ret = dwarf_read_uleb128 (as, a, addr, valp, arg)) < 0)
    return ret;

  if (*valp >= DWARF_NUM_PRESERVED_REGS)
    {
      Debug (1, "Invalid register number %u\n", (unsigned int) *valp);
      return -UNW_EBADREG;
    }
  return 0;
}

static inline void
set_reg (dwarf_state_record_t *sr, unw_word_t regnum, dwarf_where_t where,
         unw_word_t val)
{
  sr->rs_current.reg.where[regnum] = where;
  sr->rs_current.reg.val[regnum] = val;
}

static inline int
push_rstate_stack(dwarf_stackable_reg_state_t **rs_stack)
{
  dwarf_stackable_reg_state_t *old_rs = *rs_stack;
  if (NULL == (*rs_stack = alloc_reg_state ()))
    {
      *rs_stack = old_rs;
      return -1;
    }
  (*rs_stack)->next = old_rs;
  return 0;
}

static inline void
pop_rstate_stack(dwarf_stackable_reg_state_t **rs_stack)
{
  dwarf_stackable_reg_state_t *old_rs = *rs_stack;
  *rs_stack = old_rs->next;
  free_reg_state (old_rs);
}

static inline void
empty_rstate_stack(dwarf_stackable_reg_state_t **rs_stack)
{
  while (*rs_stack)
    pop_rstate_stack(rs_stack);
}

#ifdef UNW_TARGET_AARCH64

static void
aarch64_negate_ra_sign_state(dwarf_state_record_t *sr);

#endif

/* Run a CFI program to update the register state.  */
static int
run_cfi_program (struct dwarf_cursor *c, dwarf_state_record_t *sr,
                 unw_word_t *ip, unw_word_t end_ip,
		 unw_word_t *addr, unw_word_t end_addr,
		 dwarf_stackable_reg_state_t **rs_stack,
                 struct dwarf_cie_info *dci)
{
  unw_addr_space_t as;
  void *arg;

  if (c->pi.flags & UNW_PI_FLAG_DEBUG_FRAME)
    {
      /* .debug_frame CFI is stored in local address space.  */
      as = unw_local_addr_space;
      arg = NULL;
    }
  else
    {
      as = c->as;
      arg = c->as_arg;
    }
  unw_accessors_t *a = unw_get_accessors_int (as);
  int ret = 0;

  while (*ip <= end_ip && *addr < end_addr && ret >= 0)
    {
      unw_word_t operand = 0, regnum, val, len;
      uint8_t u8, op;
      uint16_t u16;
      uint32_t u32;

      if ((ret = dwarf_readu8 (as, a, addr, &op, arg)) < 0)
        break;

      if (op & DWARF_CFA_OPCODE_MASK)
        {
          operand = op & DWARF_CFA_OPERAND_MASK;
          op &= ~DWARF_CFA_OPERAND_MASK;
        }
      switch ((dwarf_cfa_t) op)
        {
        case DW_CFA_advance_loc:
          *ip += operand * dci->code_align;
          Debug (15, "CFA_advance_loc to 0x%lx\n", (long) *ip);
          break;

        case DW_CFA_advance_loc1:
          if ((ret = dwarf_readu8 (as, a, addr, &u8, arg)) < 0)
            break;
          *ip += u8 * dci->code_align;
          Debug (15, "CFA_advance_loc1 to 0x%lx\n", (long) *ip);
          break;

        case DW_CFA_advance_loc2:
          if ((ret = dwarf_readu16 (as, a, addr, &u16, arg)) < 0)
            break;
          *ip += u16 * dci->code_align;
          Debug (15, "CFA_advance_loc2 to 0x%lx\n", (long) *ip);
          break;

        case DW_CFA_advance_loc4:
          if ((ret = dwarf_readu32 (as, a, addr, &u32, arg)) < 0)
            break;
          *ip += u32 * dci->code_align;
          Debug (15, "CFA_advance_loc4 to 0x%lx\n", (long) *ip);
          break;

        case DW_CFA_MIPS_advance_loc8:
#ifdef UNW_TARGET_MIPS
          {
            uint64_t u64 = 0;

            if ((ret = dwarf_readu64 (as, a, addr, &u64, arg)) < 0)
              break;
            *ip += u64 * dci->code_align;
            Debug (15, "CFA_MIPS_advance_loc8\n");
            break;
          }
#else
          Debug (1, "DW_CFA_MIPS_advance_loc8 on non-MIPS target\n");
          ret = -UNW_EINVAL;
          break;
#endif

        case DW_CFA_offset:
          regnum = operand;
          if (regnum >= DWARF_NUM_PRESERVED_REGS)
            {
              Debug (1, "Invalid register number %u in DW_cfa_OFFSET\n",
                     (unsigned int) regnum);
              ret = -UNW_EBADREG;
              break;
            }
          if ((ret = dwarf_read_uleb128 (as, a, addr, &val, arg)) < 0)
            break;
          set_reg (sr, regnum, DWARF_WHERE_CFAREL, val * dci->data_align);
          Debug (15, "CFA_offset r%lu at cfa+0x%lx\n",
                 (long) regnum, (long) (val * dci->data_align));
          break;

        case DW_CFA_offset_extended:
          if (((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
              || ((ret = dwarf_read_uleb128 (as, a, addr, &val, arg)) < 0))
            break;
          set_reg (sr, regnum, DWARF_WHERE_CFAREL, val * dci->data_align);
          Debug (15, "CFA_offset_extended r%lu at cf+0x%lx\n",
                 (long) regnum, (long) (val * dci->data_align));
          break;

        case DW_CFA_offset_extended_sf:
          if (((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
              || ((ret = dwarf_read_sleb128 (as, a, addr, &val, arg)) < 0))
            break;
          set_reg (sr, regnum, DWARF_WHERE_CFAREL, val * dci->data_align);
          Debug (15, "CFA_offset_extended_sf r%lu at cf+0x%lx\n",
                 (long) regnum, (long) (val * dci->data_align));
          break;

        case DW_CFA_restore:
          regnum = operand;
          if (regnum >= DWARF_NUM_PRESERVED_REGS)
            {
              Debug (1, "Invalid register number %u in DW_CFA_restore\n",
                     (unsigned int) regnum);
              ret = -UNW_EINVAL;
              break;
            }
          sr->rs_current.reg.where[regnum] = sr->rs_initial.reg.where[regnum];
          sr->rs_current.reg.val[regnum] = sr->rs_initial.reg.val[regnum];
          Debug (15, "CFA_restore r%lu\n", (long) regnum);
          break;

        case DW_CFA_restore_extended:
          if ((ret = dwarf_read_uleb128 (as, a, addr, &regnum, arg)) < 0)
            break;
          if (regnum >= DWARF_NUM_PRESERVED_REGS)
            {
              Debug (1, "Invalid register number %u in "
                     "DW_CFA_restore_extended\n", (unsigned int) regnum);
              ret = -UNW_EINVAL;
              break;
            }
          sr->rs_current.reg.where[regnum] = sr->rs_initial.reg.where[regnum];
          sr->rs_current.reg.val[regnum] = sr->rs_initial.reg.val[regnum];
          Debug (15, "CFA_restore_extended r%lu\n", (long) regnum);
          break;

        case DW_CFA_nop:
          break;

        case DW_CFA_set_loc:
          if ((ret = dwarf_read_encoded_pointer (as, a, addr, dci->fde_encoding,
                                                 &c->pi, ip,
                                                 arg)) < 0)
            break;
          Debug (15, "CFA_set_loc to 0x%lx\n", (long) *ip);
          break;

        case DW_CFA_undefined:
          if ((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
            break;
          set_reg (sr, regnum, DWARF_WHERE_UNDEF, 0);
          Debug (15, "CFA_undefined r%lu\n", (long) regnum);
          break;

        case DW_CFA_same_value:
          if ((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
            break;
          set_reg (sr, regnum, DWARF_WHERE_SAME, 0);
          Debug (15, "CFA_same_value r%lu\n", (long) regnum);
          break;

        case DW_CFA_register:
          if (((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
              || ((ret = dwarf_read_uleb128 (as, a, addr, &val, arg)) < 0))
            break;
          set_reg (sr, regnum, DWARF_WHERE_REG, val);
          Debug (15, "CFA_register r%lu to r%lu\n", (long) regnum, (long) val);
          break;

        case DW_CFA_remember_state:
	  if (push_rstate_stack(rs_stack) < 0)
	    {
              Debug (1, "Out of memory in DW_CFA_remember_state\n");
              ret = -UNW_ENOMEM;
              break;
	    }
          (*rs_stack)->state = sr->rs_current;
          Debug (15, "CFA_remember_state\n");
          break;

        case DW_CFA_restore_state:
          if (!*rs_stack)
            {
              Debug (1, "register-state stack underflow\n");
              ret = -UNW_EINVAL;
              break;
            }
          sr->rs_current = (*rs_stack)->state;
          pop_rstate_stack(rs_stack);
          Debug (15, "CFA_restore_state\n");
          break;

        case DW_CFA_def_cfa:
          if (((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
              || ((ret = dwarf_read_uleb128 (as, a, addr, &val, arg)) < 0))
            break;
          set_reg (sr, DWARF_CFA_REG_COLUMN, DWARF_WHERE_REG, regnum);
          set_reg (sr, DWARF_CFA_OFF_COLUMN, 0, val);   /* NOT factored! */
          Debug (15, "CFA_def_cfa r%lu+0x%lx\n", (long) regnum, (long) val);
          break;

        case DW_CFA_def_cfa_sf:
          if (((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
              || ((ret = dwarf_read_sleb128 (as, a, addr, &val, arg)) < 0))
            break;
          set_reg (sr, DWARF_CFA_REG_COLUMN, DWARF_WHERE_REG, regnum);
          set_reg (sr, DWARF_CFA_OFF_COLUMN, 0,
                   val * dci->data_align);              /* factored! */
          Debug (15, "CFA_def_cfa_sf r%lu+0x%lx\n",
                 (long) regnum, (long) (val * dci->data_align));
          break;

        case DW_CFA_def_cfa_register:
          if ((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
            break;
          set_reg (sr, DWARF_CFA_REG_COLUMN, DWARF_WHERE_REG, regnum);
          Debug (15, "CFA_def_cfa_register r%lu\n", (long) regnum);
          break;

        case DW_CFA_def_cfa_offset:
          if ((ret = dwarf_read_uleb128 (as, a, addr, &val, arg)) < 0)
            break;
          set_reg (sr, DWARF_CFA_OFF_COLUMN, 0, val);   /* NOT factored! */
          Debug (15, "CFA_def_cfa_offset 0x%lx\n", (long) val);
          break;

        case DW_CFA_def_cfa_offset_sf:
          if ((ret = dwarf_read_sleb128 (as, a, addr, &val, arg)) < 0)
            break;
          set_reg (sr, DWARF_CFA_OFF_COLUMN, 0,
                   val * dci->data_align);      /* factored! */
          Debug (15, "CFA_def_cfa_offset_sf 0x%lx\n",
                 (long) (val * dci->data_align));
          break;

        case DW_CFA_def_cfa_expression:
          /* Save the address of the DW_FORM_block for later evaluation. */
          set_reg (sr, DWARF_CFA_REG_COLUMN, DWARF_WHERE_EXPR, *addr);

          if ((ret = dwarf_read_uleb128 (as, a, addr, &len, arg)) < 0)
            break;

          Debug (15, "CFA_def_cfa_expr @ 0x%lx [%lu bytes]\n",
                 (long) *addr, (long) len);
          *addr += len;
          break;

        case DW_CFA_expression:
          if ((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
            break;

          /* Save the address of the DW_FORM_block for later evaluation. */
          set_reg (sr, regnum, DWARF_WHERE_EXPR, *addr);

          if ((ret = dwarf_read_uleb128 (as, a, addr, &len, arg)) < 0)
            break;

          Debug (15, "CFA_expression r%lu @ 0x%lx [%lu bytes]\n",
                 (long) regnum, (long) addr, (long) len);
          *addr += len;
          break;

        case DW_CFA_val_expression:
          if ((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
            break;

          /* Save the address of the DW_FORM_block for later evaluation. */
          set_reg (sr, regnum, DWARF_WHERE_VAL_EXPR, *addr);

          if ((ret = dwarf_read_uleb128 (as, a, addr, &len, arg)) < 0)
            break;

          Debug (15, "CFA_val_expression r%lu @ 0x%lx [%lu bytes]\n",
                 (long) regnum, (long) addr, (long) len);
          *addr += len;
          break;

        case DW_CFA_GNU_args_size:
          if ((ret = dwarf_read_uleb128 (as, a, addr, &val, arg)) < 0)
            break;
          sr->args_size = val;
          Debug (15, "CFA_GNU_args_size %lu\n", (long) val);
          break;

        case DW_CFA_GNU_negative_offset_extended:
          /* A comment in GCC says that this is obsoleted by
             DW_CFA_offset_extended_sf, but that it's used by older
             PowerPC code.  */
          if (((ret = read_regnum (as, a, addr, &regnum, arg)) < 0)
              || ((ret = dwarf_read_uleb128 (as, a, addr, &val, arg)) < 0))
            break;
          set_reg (sr, regnum, DWARF_WHERE_CFAREL, ~(val * dci->data_align) + 1);
          Debug (15, "CFA_GNU_negative_offset_extended cfa+0x%lx\n",
                 (long) (~(val * dci->data_align) + 1));
          break;

        case DW_CFA_GNU_window_save:
#ifdef UNW_TARGET_SPARC
          /* This is a special CFA to handle all 16 windowed registers
             on SPARC.  */
          for (regnum = 16; regnum < 32; ++regnum)
            set_reg (sr, regnum, DWARF_WHERE_CFAREL,
                     (regnum - 16) * sizeof (unw_word_t));
          Debug (15, "CFA_GNU_window_save\n");
          break;
#elif UNW_TARGET_AARCH64
          /* This is a specific opcode on aarch64, DW_CFA_AARCH64_negate_ra_state */
          Debug (15, "DW_CFA_AARCH64_negate_ra_state\n");
          aarch64_negate_ra_sign_state(sr);
          break;
#else
          /* FALL THROUGH */
#endif
        case DW_CFA_lo_user:
        case DW_CFA_hi_user:
          Debug (1, "Unexpected CFA opcode 0x%x\n", op);
          ret = -UNW_EINVAL;
          break;
        }
    }

  if (ret > 0)
    ret = 0;
  return ret;
}

static int
fetch_proc_info (struct dwarf_cursor *c, unw_word_t ip)
{
  int ret, dynamic = 1;

  /* The 'ip' can point either to the previous or next instruction
     depending on what type of frame we have: normal call or a place
     to resume execution (e.g. after signal frame).

     For a normal call frame we need to back up so we point within the
     call itself; this is important because a) the call might be the
     very last instruction of the function and the edge of the FDE,
     and b) so that run_cfi_program() runs locations up to the call
     but not more.

     For signal frame, we need to do the exact opposite and look
     up using the current 'ip' value.  That is where execution will
     continue, and it's important we get this right, as 'ip' could be
     right at the function entry and hence FDE edge, or at instruction
     that manipulates CFA (push/pop). */

  if (c->use_prev_instr)
    {
#if defined(__arm__)
      /* On arm, the least bit denotes thumb/arm mode, clear it. */
      ip &= ~(unw_word_t)0x1;
#endif
      --ip;
    }

  memset (&c->pi, 0, sizeof (c->pi));

  /* check dynamic info first --- it overrides everything else */
  ret = unwi_find_dynamic_proc_info (c->as, ip, &c->pi, 1,
                                     c->as_arg);
  if (ret == -UNW_ENOINFO)
    {
      dynamic = 0;
      if ((ret = tdep_find_proc_info (c, ip, 1)) < 0)
        return ret;
    }

  if (c->pi.format != UNW_INFO_FORMAT_DYNAMIC
      && c->pi.format != UNW_INFO_FORMAT_TABLE
      && c->pi.format != UNW_INFO_FORMAT_REMOTE_TABLE)
    return -UNW_ENOINFO;

  c->pi_valid = 1;
  c->pi_is_dynamic = dynamic;

  /* Let system/machine-dependent code determine frame-specific attributes. */
  if (ret >= 0)
    tdep_fetch_frame (c, ip, 1);

  return ret;
}

static int
parse_dynamic (struct dwarf_cursor  *c UNUSED,
			   unw_word_t            ip UNUSED,
               dwarf_state_record_t *sr UNUSED)
{
  Debug (1, "Not yet implemented\n");
  return -UNW_ENOINFO;
}

static inline void
put_unwind_info (struct dwarf_cursor *c, unw_proc_info_t *pi)
{
  if (c->pi_is_dynamic)
    unwi_put_dynamic_unwind_info (c->as, pi, c->as_arg);
  else if (pi->unwind_info && pi->format == UNW_INFO_FORMAT_TABLE)
    {
      mempool_free (&dwarf_cie_info_pool, pi->unwind_info);
      pi->unwind_info = NULL;
    }
  c->pi_valid = 0;
}

static inline int
setup_fde (struct dwarf_cursor *c, dwarf_state_record_t *sr)
{
  int i, ret;

  assert (c->pi_valid);

  memset (sr, 0, sizeof (*sr));
  for (i = 0; i < DWARF_NUM_PRESERVED_REGS + 2; ++i)
    set_reg (sr, i, DWARF_WHERE_SAME, 0);

#if !defined(UNW_TARGET_ARM) && !(defined(UNW_TARGET_MIPS) && _MIPS_SIM == _ABI64)
  // SP defaults to CFA (but is overridable)
  set_reg (sr, TDEP_DWARF_SP, DWARF_WHERE_CFA, 0);
#endif

  struct dwarf_cie_info *dci = c->pi.unwind_info;
  sr->rs_current.ret_addr_column  = dci->ret_addr_column;
  unw_word_t addr = dci->cie_instr_start;
  unw_word_t curr_ip = 0;
  dwarf_stackable_reg_state_t *rs_stack = NULL;
  ret = run_cfi_program (c, sr, &curr_ip, ~(unw_word_t) 0, &addr,
			 dci->cie_instr_end,
			 &rs_stack, dci);
  empty_rstate_stack(&rs_stack);
  if (ret < 0)
    return ret;

  memcpy (&sr->rs_initial, &sr->rs_current, sizeof (sr->rs_initial));
  return 0;
}

static inline int
parse_fde (struct dwarf_cursor *c, unw_word_t ip, dwarf_state_record_t *sr)
{
  int ret;
  struct dwarf_cie_info *dci = c->pi.unwind_info;
  unw_word_t addr = dci->fde_instr_start;
  unw_word_t curr_ip = c->pi.start_ip;
  dwarf_stackable_reg_state_t *rs_stack = NULL;
  /* Process up to current `ip` for signal frame and `ip - 1` for normal call frame
     See `c->use_prev_instr` use in `fetch_proc_info` for details. */
  ret = run_cfi_program (c, sr, &curr_ip, ip - c->use_prev_instr, &addr, dci->fde_instr_end,
			 &rs_stack, dci);
  empty_rstate_stack(&rs_stack);
  if (ret < 0)
    return ret;

  return 0;
}

HIDDEN int
dwarf_flush_rs_cache (struct dwarf_rs_cache *cache)
{
  int i;

  if (cache->log_size == DWARF_DEFAULT_LOG_UNW_CACHE_SIZE
      || !cache->hash) {
    cache->hash = cache->default_hash;
    cache->buckets = cache->default_buckets;
    cache->links = cache->default_links;
    cache->log_size = DWARF_DEFAULT_LOG_UNW_CACHE_SIZE;
  } else {
    if (cache->hash && cache->hash != cache->default_hash)
      mi_munmap(cache->hash, DWARF_UNW_HASH_SIZE(cache->prev_log_size)
                              * sizeof (cache->hash[0]));
    if (cache->buckets && cache->buckets != cache->default_buckets)
      mi_munmap(cache->buckets, DWARF_UNW_CACHE_SIZE(cache->prev_log_size)
                              * sizeof (cache->buckets[0]));
    if (cache->links && cache->links != cache->default_links)
      mi_munmap(cache->links, DWARF_UNW_CACHE_SIZE(cache->prev_log_size)
                              * sizeof (cache->links[0]));
    GET_MEMORY(cache->hash, DWARF_UNW_HASH_SIZE(cache->log_size)
                             * sizeof (cache->hash[0]));
    GET_MEMORY(cache->buckets, DWARF_UNW_CACHE_SIZE(cache->log_size)
                                * sizeof (cache->buckets[0]));
    GET_MEMORY(cache->links, DWARF_UNW_CACHE_SIZE(cache->log_size)
                                * sizeof (cache->links[0]));
    if (!cache->hash || !cache->buckets || !cache->links)
      {
        Debug (1, "Unable to allocate cache memory");
        return -UNW_ENOMEM;
      }
    cache->prev_log_size = cache->log_size;
  }

  cache->rr_head = 0;

  for (i = 0; i < DWARF_UNW_CACHE_SIZE(cache->log_size); ++i)
    {
      cache->links[i].coll_chain = -1;
      cache->links[i].ip = 0;
      cache->links[i].valid = 0;
    }
  for (i = 0; i< DWARF_UNW_HASH_SIZE(cache->log_size); ++i)
    cache->hash[i] = -1;

  return 0;
}

static inline struct dwarf_rs_cache *
get_rs_cache (unw_addr_space_t as, intrmask_t *saved_maskp)
{
  struct dwarf_rs_cache *cache = &as->global_cache;
  unw_caching_policy_t caching = as->caching_policy;

  if (caching == UNW_CACHE_NONE)
    return NULL;

#if defined(HAVE___CACHE_PER_THREAD) && HAVE___CACHE_PER_THREAD
  if (likely (caching == UNW_CACHE_PER_THREAD))
    {
      static _Thread_local struct dwarf_rs_cache tls_cache __attribute__((tls_model("initial-exec")));
      Debug (16, "using TLS cache\n");
      cache = &tls_cache;
    }
  else
#else
  if (likely (caching == UNW_CACHE_GLOBAL))
#endif
    {
      Debug (16, "acquiring lock\n");
      lock_acquire (&cache->lock, *saved_maskp);
    }

  if ((atomic_load (&as->cache_generation) != atomic_load (&cache->generation))
       || !cache->hash)
    {
      /* cache_size is only set in the global_cache, copy it over before flushing */
      cache->log_size = as->global_cache.log_size;
      if (dwarf_flush_rs_cache (cache) < 0)
        return NULL;
      atomic_store (&cache->generation, atomic_load (&as->cache_generation));
    }

  return cache;
}

static inline void
put_rs_cache (unw_addr_space_t as, struct dwarf_rs_cache *cache,
                  intrmask_t *saved_maskp)
{
  assert (as->caching_policy != UNW_CACHE_NONE);

  Debug (16, "unmasking signals/interrupts and releasing lock\n");
  if (likely (as->caching_policy == UNW_CACHE_GLOBAL))
    lock_release (&cache->lock, *saved_maskp);
}

static inline unw_hash_index_t CONST_ATTR
hash (unw_word_t ip, unsigned short log_size)
{
  /* based on (sqrt(5)/2-1)*2^64 */
# define magic  ((unw_word_t) 0x9e3779b97f4a7c16ULL)

  return (unw_hash_index_t) (ip * magic >> ((sizeof(unw_word_t) * 8) - (log_size + 1)));
}

static inline long
cache_match (struct dwarf_rs_cache *cache, unsigned short index, unw_word_t ip)
{
  return (cache->links[index].valid && (ip == cache->links[index].ip));
}

static dwarf_reg_state_t *
rs_lookup (struct dwarf_rs_cache *cache, struct dwarf_cursor *c)
{
  unsigned short index;
  unw_word_t ip = c->ip;

  if (c->hint > 0)
    {
      index = c->hint - 1;
      if (cache_match (cache, index, ip))
	return &cache->buckets[index];
    }

  for (index = cache->hash[hash (ip, cache->log_size)];
       index < DWARF_UNW_CACHE_SIZE(cache->log_size);
       index = cache->links[index].coll_chain)
    {
      if (cache_match (cache, index, ip))
	return &cache->buckets[index];
    }
  return NULL;
}

static inline dwarf_reg_state_t *
rs_new (struct dwarf_rs_cache *cache, struct dwarf_cursor * c)
{
  unw_hash_index_t index;
  unsigned short head;

  head = cache->rr_head;
  cache->rr_head = (head + 1) & (DWARF_UNW_CACHE_SIZE(cache->log_size) - 1);

  /* remove the old rs from the hash table (if it's there): */
  if (cache->links[head].ip)
    {
      unsigned short *pindex;
      for (pindex = &cache->hash[hash (cache->links[head].ip, cache->log_size)];
	   *pindex < DWARF_UNW_CACHE_SIZE(cache->log_size);
	   pindex = &cache->links[*pindex].coll_chain)
	{
	  if (*pindex == head)
	    {
	      *pindex = cache->links[*pindex].coll_chain;
	      break;
	    }
	}
    }

  /* enter new rs in the hash table */
  index = hash (c->ip, cache->log_size);
  cache->links[head].coll_chain = cache->hash[index];
  cache->hash[index] = head;

  cache->links[head].ip = c->ip;
  cache->links[head].valid = 1;
  cache->links[head].signal_frame = tdep_cache_frame(c) ? 1 : 0;
  return cache->buckets + head;
}

static int
create_state_record_for (struct dwarf_cursor *c, dwarf_state_record_t *sr,
                         unw_word_t ip)
{
  int ret;
  switch (c->pi.format)
    {
    case UNW_INFO_FORMAT_TABLE:
    case UNW_INFO_FORMAT_REMOTE_TABLE:
      if ((ret = setup_fde(c, sr)) < 0)
	return ret;
      ret = parse_fde (c, ip, sr);
      break;

    case UNW_INFO_FORMAT_DYNAMIC:
      ret = parse_dynamic (c, ip, sr);
      break;

    default:
      Debug (1, "Unexpected unwind-info format %d\n", c->pi.format);
      ret = -UNW_EINVAL;
    }
  return ret;
}

static inline int
eval_location_expr (struct dwarf_cursor *c, unw_word_t stack_val, unw_addr_space_t as,
                    unw_accessors_t *a, unw_word_t addr,
                    dwarf_loc_t *locp, void *arg)
{
  int ret, is_register;
  unw_word_t len, val;

  /* read the length of the expression: */
  if ((ret = dwarf_read_uleb128 (as, a, &addr, &len, arg)) < 0)
    return ret;

  /* evaluate the expression: */
  if ((ret = dwarf_eval_expr (c, stack_val, &addr, len, &val, &is_register)) < 0)
    return ret;

  if (is_register)
    *locp = DWARF_REG_LOC (c, dwarf_to_unw_regnum (val));
  else
    *locp = DWARF_MEM_LOC (c, val);

  return 0;
}


#ifdef UNW_TARGET_AARCH64
#include "libunwind-aarch64.h"

static void
aarch64_negate_ra_sign_state(dwarf_state_record_t *sr)
{
  unw_word_t ra_sign_state = sr->rs_current.reg.val[UNW_AARCH64_RA_SIGN_STATE];
  ra_sign_state ^= 0x1;
  set_reg(sr, UNW_AARCH64_RA_SIGN_STATE, DWARF_WHERE_SAME, ra_sign_state);
}

static unw_word_t
aarch64_get_ra_sign_state(struct dwarf_reg_state *rs)
{
   return rs->reg.val[UNW_AARCH64_RA_SIGN_STATE];
}

#endif

static int
apply_reg_state (struct dwarf_cursor *c, struct dwarf_reg_state *rs)
{
  unw_regnum_t regnum;
  unw_word_t addr, cfa, ip;
  unw_word_t prev_ip, prev_cfa;
  unw_addr_space_t as;
  dwarf_loc_t cfa_loc;
  unw_accessors_t *a;
  int i, ret;
  void *arg;

  /* In the case that we have incorrect CFI, the return address column may be
   * outside the valid range of data and will read invalid data.  Protect
   * against the errant read and indicate that we have a bad frame.  */
  if (rs->ret_addr_column >= DWARF_NUM_PRESERVED_REGS) {
    Dprintf ("%s: return address entry %zu is outside of range of CIE",
             __FUNCTION__, rs->ret_addr_column);
    return -UNW_EBADFRAME;
  }

  prev_ip = c->ip;
  prev_cfa = c->cfa;

  as = c->as;
  arg = c->as_arg;
  a = unw_get_accessors_int (as);

  /* Evaluate the CFA first, because it may be referred to by other
     expressions.  */

  if (rs->reg.where[DWARF_CFA_REG_COLUMN] == DWARF_WHERE_REG)
    {
      /* CFA is equal to [reg] + offset: */

      /* As a special-case, if the stack-pointer is the CFA and the
         stack-pointer wasn't saved, popping the CFA implicitly pops
         the stack-pointer as well.  */
      if ((rs->reg.val[DWARF_CFA_REG_COLUMN] == TDEP_DWARF_SP)
          && (TDEP_DWARF_SP < ARRAY_SIZE(rs->reg.val))
          && (DWARF_IS_NULL_LOC(c->loc[TDEP_DWARF_SP])))
          cfa = c->cfa;
      else
        {
          regnum = dwarf_to_unw_regnum ((unw_regnum_t) rs->reg.val[DWARF_CFA_REG_COLUMN]);
          if ((ret = unw_get_reg (dwarf_to_cursor(c), regnum, &cfa)) < 0)
            return ret;
        }
      cfa += rs->reg.val[DWARF_CFA_OFF_COLUMN];
    }
  else
    {
      /* CFA is equal to EXPR: */

      assert (rs->reg.where[DWARF_CFA_REG_COLUMN] == DWARF_WHERE_EXPR);

      addr = rs->reg.val[DWARF_CFA_REG_COLUMN];
      /* The dwarf standard doesn't specify an initial value to be pushed on */
      /* the stack before DW_CFA_def_cfa_expression evaluation. We push on a */
      /* dummy value (0) to keep the eval_location_expr function consistent. */
      if ((ret = eval_location_expr (c, 0, as, a, addr, &cfa_loc, arg)) < 0)
        return ret;
      /* the returned location better be a memory location... */
      if (DWARF_IS_REG_LOC (cfa_loc))
        return -UNW_EBADFRAME;
      cfa = DWARF_GET_LOC (cfa_loc);
    }

  dwarf_loc_t new_loc[DWARF_NUM_PRESERVED_REGS];
  memcpy(new_loc, c->loc, sizeof(new_loc));

  for (i = 0; i < DWARF_NUM_PRESERVED_REGS; ++i)
    {
      switch ((dwarf_where_t) rs->reg.where[i])
        {
        case DWARF_WHERE_UNDEF:
          new_loc[i] = DWARF_NULL_LOC;
          break;

        case DWARF_WHERE_SAME:
          break;

        case DWARF_WHERE_CFA:
          new_loc[i] = DWARF_VAL_LOC (c, cfa);
          break;

        case DWARF_WHERE_CFAREL:
          new_loc[i] = DWARF_MEM_LOC (c, cfa + rs->reg.val[i]);
          break;

        case DWARF_WHERE_REG:
#ifdef __s390x__
          /* GPRs can be saved in FPRs on s390x */
          if (unw_is_fpreg (dwarf_to_unw_regnum (rs->reg.val[i])))
            {
              new_loc[i] = DWARF_FPREG_LOC (c, dwarf_to_unw_regnum (rs->reg.val[i]));
              break;
            }
#endif
          new_loc[i] = new_loc[rs->reg.val[i]];
          break;

        case DWARF_WHERE_EXPR:
          addr = rs->reg.val[i];
          /* The dwarf standard requires the current CFA to be pushed on the */
          /* stack before DW_CFA_expression evaluation. */
          if ((ret = eval_location_expr (c, cfa, as, a, addr, new_loc + i, arg)) < 0)
            return ret;
          break;

        case DWARF_WHERE_VAL_EXPR:
          addr = rs->reg.val[i];
          /* The dwarf standard requires the current CFA to be pushed on the */
          /* stack before DW_CFA_val_expression evaluation. */
          if ((ret = eval_location_expr (c, cfa, as, a, addr, new_loc + i, arg)) < 0)
            return ret;
          new_loc[i] = DWARF_VAL_LOC (c, DWARF_GET_LOC (new_loc[i]));
          break;
        }
    }

  memcpy(c->loc, new_loc, sizeof(new_loc));

  c->cfa = cfa;
  /* DWARF spec says undefined return address location means end of stack. */
  if (DWARF_IS_NULL_LOC (c->loc[rs->ret_addr_column]))
    {
      c->ip = 0;
    }
  else
  {
    ret = dwarf_get (c, c->loc[rs->ret_addr_column], &ip);
    if (ret < 0)
      return ret;
#ifdef UNW_TARGET_AARCH64
    if (aarch64_get_ra_sign_state(rs))
      {
        ip = tdep_strip_ptrauth_insn_mask ((unw_cursor_t*)c, ip);
      }
#endif
    c->ip = ip;
  }
  ret = (c->ip != 0) ? 1 : 0;

  /* XXX: check for ip to be code_aligned */
  if (c->ip == prev_ip && c->cfa == prev_cfa)
    {
      Dprintf ("%s: ip and cfa unchanged; stopping here (ip=0x%lx)\n",
               __FUNCTION__, (long) c->ip);
      return -UNW_EBADFRAME;
    }

  if (c->stash_frames)
    tdep_stash_frame (c, rs);

  return ret;
}

/* Find the saved locations. */
static int
find_reg_state (struct dwarf_cursor *c, dwarf_state_record_t *sr)
{
  dwarf_reg_state_t *rs = NULL;
  struct dwarf_rs_cache *cache;
  int ret = 0;
  intrmask_t saved_mask;

  if ((cache = get_rs_cache(c->as, &saved_mask)) &&
      (rs = rs_lookup(cache, c)))
    {
      /* update hint; no locking needed: single-word writes are atomic */
      unsigned short index = (unsigned short) (rs - cache->buckets);
      c->use_prev_instr = ! cache->links[index].signal_frame;
      memcpy (&sr->rs_current, rs, sizeof (*rs));
    }
  else
    {
      ret = fetch_proc_info (c, c->ip);
      int next_use_prev_instr = c->use_prev_instr;
      if (ret >= 0)
	{
	  /* Update use_prev_instr for the next frame. */
	  assert(c->pi.unwind_info);
	  struct dwarf_cie_info *dci = c->pi.unwind_info;
	  next_use_prev_instr = ! dci->signal_frame;
	  ret = create_state_record_for (c, sr, c->ip);
	}
      put_unwind_info (c, &c->pi);
      c->use_prev_instr = next_use_prev_instr;

      if (cache && ret >= 0)
	{
	  rs = rs_new (cache, c);
	  cache->links[rs - cache->buckets].hint = 0;
	  memcpy(rs, &sr->rs_current, sizeof(*rs));
	}
    }

  unsigned short index = -1;
  if (cache)
    {
      if (rs)
	{
	  index = (unsigned short) (rs - cache->buckets);
	  c->hint = cache->links[index].hint;
	  cache->links[c->prev_rs].hint = index + 1;
	  c->prev_rs = index;
	}
      if (ret >= 0)
        tdep_reuse_frame (c, cache->links[index].signal_frame);
      put_rs_cache (c->as, cache, &saved_mask);
    }
  return ret;
}

/* The function finds the saved locations and applies the register
   state as well. */
HIDDEN int
dwarf_step (struct dwarf_cursor *c)
{
  int ret;
  dwarf_state_record_t sr;
  if ((ret = find_reg_state (c, &sr)) < 0)
    return ret;
  return apply_reg_state (c, &sr.rs_current);
}

HIDDEN int
dwarf_make_proc_info (struct dwarf_cursor *c)
{
#if 0
  if (c->as->caching_policy == UNW_CACHE_NONE
      || get_cached_proc_info (c) < 0)
#endif
  /* Need to check if current frame contains
     args_size, and set cursor appropriately.  Only
     needed for unw_resume */
  dwarf_state_record_t sr;
  sr.args_size = 0;
  int ret;

  /* Lookup it up the slow way... */
  ret = fetch_proc_info (c, c->ip);
  if (ret >= 0)
      ret = create_state_record_for (c, &sr, c->ip);
  put_unwind_info (c, &c->pi);
  if (ret < 0)
    return ret;
  c->args_size = sr.args_size;

  return 0;
}

static int
dwarf_reg_states_dynamic_iterate(struct dwarf_cursor     *c UNUSED,
                                 unw_reg_states_callback  cb UNUSED,
                                 void                    *token UNUSED)
{
  Debug (1, "Not yet implemented\n");
  return -UNW_ENOINFO;
}

static int
dwarf_reg_states_table_iterate(struct dwarf_cursor *c,
			       unw_reg_states_callback cb,
			       void *token)
{
  dwarf_state_record_t sr;
  int ret = setup_fde(c, &sr);
  struct dwarf_cie_info *dci = c->pi.unwind_info;
  unw_word_t addr = dci->fde_instr_start;
  unw_word_t curr_ip = c->pi.start_ip;
  dwarf_stackable_reg_state_t *rs_stack = NULL;
  while (ret >= 0 && curr_ip < c->pi.end_ip && addr < dci->fde_instr_end)
    {
      unw_word_t prev_ip = curr_ip;
      ret = run_cfi_program (c, &sr, &curr_ip, prev_ip, &addr, dci->fde_instr_end,
			     &rs_stack, dci);
      if (ret >= 0 && prev_ip < curr_ip)
	ret = cb(token, &sr.rs_current, sizeof(sr.rs_current), prev_ip, curr_ip);
    }
  empty_rstate_stack(&rs_stack);
#if defined(NEED_LAST_IP)
  if (ret >= 0 && curr_ip < c->pi.last_ip)
    /* report the dead zone after the procedure ends */
    ret = cb(token, &sr.rs_current, sizeof(sr.rs_current), curr_ip, c->pi.last_ip);
#else
  if (ret >= 0 && curr_ip < c->pi.end_ip)
    /* report for whatever is left before procedure end */
    ret = cb(token, &sr.rs_current, sizeof(sr.rs_current), curr_ip, c->pi.end_ip);
#endif
  return ret;
}

HIDDEN int
dwarf_reg_states_iterate(struct dwarf_cursor *c,
			 unw_reg_states_callback cb,
			 void *token)
{
  int ret = fetch_proc_info (c, c->ip);
  int next_use_prev_instr = c->use_prev_instr;
  if (ret >= 0)
    {
      /* Update use_prev_instr for the next frame. */
      assert(c->pi.unwind_info);
      struct dwarf_cie_info *dci = c->pi.unwind_info;
      next_use_prev_instr = ! dci->signal_frame;
      switch (c->pi.format)
	{
	case UNW_INFO_FORMAT_TABLE:
	case UNW_INFO_FORMAT_REMOTE_TABLE:
	  ret = dwarf_reg_states_table_iterate(c, cb, token);
	  break;

	case UNW_INFO_FORMAT_DYNAMIC:
	  ret = dwarf_reg_states_dynamic_iterate (c, cb, token);
	  break;

	default:
	  Debug (1, "Unexpected unwind-info format %d\n", c->pi.format);
	  ret = -UNW_EINVAL;
	}
    }
  put_unwind_info (c, &c->pi);
  c->use_prev_instr = next_use_prev_instr;
  return ret;
}

HIDDEN int
dwarf_apply_reg_state (struct dwarf_cursor *c, struct dwarf_reg_state *rs)
{
  return apply_reg_state(c, rs);
}
