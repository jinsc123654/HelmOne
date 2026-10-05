/**
 * @file ftsystem.c
 * @brief 板级抽换 `apps/external/freetype/.../builds/nuttx/ftsystem.c`。
 *
 * CMake 从 lib `freetype` 去掉上游同名源，改编本文件。相对上游：
 * `ft_alloc` / `ft_realloc` / `ft_free` 走 `board_malloc_psram`，
 * 避免 FreeType 光栅在 SRAM 上 OOM。不修改 `apps/external/freetype`。
 *
 * SPDX-License-Identifier: FTL
 */

/****************************************************************************
 *
 * ftsystem.c
 *
 *   ANSI-specific FreeType low-level system interface (body).
 *
 * Copyright (C) 2024 by Xiaomi Inc.
 *
 * This file is part of the FreeType project, and may only be used,
 * modified, and distributed under the terms of the FreeType project
 * license, LICENSE.TXT.  By continuing to use, modify, or distribute
 * this file you indicate that you have read the license and
 * understand and accept it fully.
 *
 */

#include <ft2build.h>

/* ---- 抽换件标记（2026-09-18）----------------------------------------------
 * 1) 构建日志里可见：`ninja | grep "override compiled"` 能列出本次构建真的编译了
 *    哪些抽换件 —— 2026-09-18 曾有个抽换件其实根本不在编译库里（已剔除），
 *    改了半天没生效，就靠这种标记一眼看出来；
 * 2) 镜像里可查：`strings nuttx | grep vela_override/`（本符号 used，不会被
 *    --gc-sections 丢掉），不依赖任何编译选项（有些目标带 -w，会把 #warning 压掉）。
 * 见 docs/pitch/README.md。 */
/* ---------------------------------------------------------------------------
 * 抽换件说明（vela_override）
 *   替的是上游 : external/freetype/freetype / src/base/ftsystem.c
 *   写入时 HEAD: b9d36e18cf133145256eff866c0477083b2ce01e
 *   上游 blob  : 9beb7e245d2f38d4f870f815d99c1991aaba36d0     （git -C external/freetype/freetype rev-parse HEAD:src/base/ftsystem.c 应等于它）
 *   为什么抽换 : FT heap 走 PSRAM
 *   版本漂移自查:
 *     git -C external/freetype/freetype rev-parse HEAD:src/base/ftsystem.c   # 与上面的 blob 比对
 *     git -C external/freetype/freetype diff -- src/base/ftsystem.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/freetype/ftsystem.c -- 上游 external/freetype/freetype:src/base/ftsystem.c@9beb7e245d2f -- FT heap 走 PSRAM")
const char myvendor_override_marker_freetype_ftsystem_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/freetype/ftsystem.c -- 上游 external/freetype/freetype:src/base/ftsystem.c@9beb7e245d2f -- FT heap 走 PSRAM";
#include FT_CONFIG_CONFIG_H
#include <freetype/internal/ftdebug.h>
#include <freetype/internal/ftstream.h>
#include <freetype/ftsystem.h>
#include <freetype/fterrors.h>
#include <freetype/fttypes.h>
#include <nuttx/config.h>
#include <nuttx/sched_note.h>
#include <nuttx/mm/mm.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC
#include "board_malloc.h"
#endif
#ifdef CONFIG_LIB_FREETYPE_MBFC
#include "mbfc.h"
#endif

#ifdef CONFIG_TRACE_FS
#  define FT_TRACE_BEGIN sched_note_beginex(NOTE_TAG_FS, __func__)
#  define FT_TRACE_END sched_note_endex(NOTE_TAG_FS, __func__)
#else
#  define FT_TRACE_BEGIN
#  define FT_TRACE_END
#endif

  /**************************************************************************
   *
   *                      MEMORY MANAGEMENT INTERFACE
   *
   */

  /**************************************************************************
   *
   * It is not necessary to do any error checking for the
   * allocation-related functions.  This will be done by the higher level
   * routines like ft_mem_alloc() or ft_mem_realloc().
   *
   */


  /**************************************************************************
   *
   * @struct:
   *   FT_MemoryEx
   *
   * @description:
   *   A structure used to describe a custom memory.
   *
   * @fields:
   *   FT_MemoryRec_ ::
   *     A structure used to describe a given memory manager to FreeType~2.
   *
   *   buffer ::
   *     A pointer to custom buffer.
   *
   */
  typedef struct
  {
    struct FT_MemoryRec_ memory;
    void*                buffer;
  } FT_MemoryEx;


  /**************************************************************************
   *
   * @Function:
   *   ft_alloc
   *
   * @Description:
   *   The memory allocation function.
   *
   * @Input:
   *   memory ::
   *     A pointer to the memory object.
   *
   *   size ::
   *     The requested size in bytes.
   *
   * @Return:
   *   The address of newly allocated block.
   */
  FT_CALLBACK_DEF( void* )
  ft_alloc( FT_Memory  memory,
            long       size )
  {
    if ( size <= 0 )
      {
        size = 1;
      }
#ifdef CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC
    FT_UNUSED( memory );
    return board_malloc_psram( (size_t)size );
#elif CONFIG_LIB_FREETYPE_MEMSIZE
    struct mm_heap_s* heap = memory->user;
    return mm_malloc( heap, (size_t)size );
#else
    FT_UNUSED( memory );
    return malloc( (size_t)size );
#endif
  }


  /**************************************************************************
   *
   * @Function:
   *   ft_realloc
   *
   * @Description:
   *   The memory reallocation function.
   *
   * @Input:
   *   memory ::
   *     A pointer to the memory object.
   *
   *   cur_size ::
   *     The current size of the allocated memory block.
   *
   *   new_size ::
   *     The newly requested size in bytes.
   *
   *   block ::
   *     The current address of the block in memory.
   *
   * @Return:
   *   The address of the reallocated memory block.
   */
  FT_CALLBACK_DEF( void* )
  ft_realloc( FT_Memory  memory,
              long       cur_size,
              long       new_size,
              void*      block )
  {
    FT_UNUSED( cur_size );

#ifdef CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC
    FT_UNUSED( memory );
    return board_realloc_psram( block, (size_t)new_size );
#elif CONFIG_LIB_FREETYPE_MEMSIZE
    struct mm_heap_s* heap = memory->user;
    return mm_realloc( heap, block, (size_t)new_size );
#else
    FT_UNUSED( memory );
    return realloc( block, (size_t)new_size );
#endif
  }


  /**************************************************************************
   *
   * @Function:
   *   ft_free
   *
   * @Description:
   *   The memory release function.
   *
   * @Input:
   *   memory ::
   *     A pointer to the memory object.
   *
   *   block ::
   *     The address of block in memory to be freed.
   */
  FT_CALLBACK_DEF( void )
  ft_free( FT_Memory  memory,
           void*      block )
  {
#ifdef CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC
    FT_UNUSED( memory );
    board_free_psram( block );
#elif CONFIG_LIB_FREETYPE_MEMSIZE
    struct mm_heap_s* heap = memory->user;
    mm_free( heap, block );
#else
    FT_UNUSED( memory );
    free( block );
#endif
  }


  /**************************************************************************
   *
   *                    RESOURCE MANAGEMENT INTERFACE
   *
   */

#ifndef FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT

  /**************************************************************************
   *
   * The macro FT_COMPONENT is used in trace mode.  It is an implicit
   * parameter of the FT_TRACE() and FT_ERROR() macros, used to print/log
   * messages during execution.
   */
#undef  FT_COMPONENT
#define FT_COMPONENT  io

  /* We use the macro STREAM_PTR for convenience to extract the           */
  /* system-specific stream handle from a given FreeType stream object.   */
  /* The reason for 'fd + 1' is because open() may return a legal fd with */
  /* a value of 0, preventing it from being judged as NULL when converted */
  /* to a pointer type                                                    */

#define FD_TO_PTR( fd )             ( (void *)(intptr_t)( fd + 1 ) )
#define PTR_TO_FD( ptr )            ( (int)(intptr_t)ptr - 1 )
#define STREAM_PTR_TO_FD( stream )  PTR_TO_FD( stream->descriptor.pointer )

#ifdef CONFIG_LIB_FREETYPE_MBFC

#define STREAM_TO_MBFC( stream )    ((mbfc_t *)stream->descriptor.pointer)

/* Mult-block file cache */
static ssize_t mbfc_read_cb(void* fp, void* buf, size_t nbytes)
{
  return read(PTR_TO_FD(fp), buf, nbytes);
}

static off_t mbfc_seek_cb(void* fp, off_t pos, int whence)
{
  return lseek(PTR_TO_FD(fp), pos, whence);
}

  /**************************************************************************
   *
   * @Function:
   *   ft_mbfc_stream_close
   *
   * @Description:
   *   The function to close a stream.
   *
   * @Input:
   *   stream ::
   *     A pointer to the multi-block file cache stream object.
   */
  FT_CALLBACK_DEF( void )
  ft_mbfc_stream_close( FT_Stream  stream )
  {
    mbfc_t *mbfc = STREAM_TO_MBFC(stream);
    int fd = (int)(intptr_t)mbfc->param.fp;
    mbfc_delete(mbfc);
    close(fd);

    stream->descriptor.pointer = NULL;
    stream->size               = 0;
    stream->base               = NULL;
  }


/**************************************************************************
   *
   * @Function:
   *   ft_mbfc_stream_io
   *
   * @Description:
   *   The function to open a stream.
   *
   * @Input:
   *   stream ::
   *     A pointer to the stream object.
   *
   *   offset ::
   *     The position in the data stream to start reading.
   *
   *   buffer ::
   *     The address of buffer to store the read data.
   *
   *   count ::
   *     The number of bytes to read from the stream.
   *
   * @Return:
   *   The number of bytes actually read.  If `count' is zero (this is,
   *   the function is used for seeking), a non-zero return value
   *   indicates an error.
   */
  FT_CALLBACK_DEF( unsigned long )
  ft_mbfc_stream_io( FT_Stream       stream,
                   unsigned long   offset,
                   unsigned char*  buffer,
                   unsigned long   count )
  {
    if ( !count && offset > stream->size )
      return 1;

    mbfc_t *mbfc = STREAM_TO_MBFC(stream);
    ssize_t br = mbfc_read(mbfc, offset, buffer, count);

    return br > 0 ? (unsigned long)br : 0;
  }
#else

  /**************************************************************************
   *
   * @Function:
   *   ft_posix_stream_close
   *
   * @Description:
   *   The function to close a stream.
   *
   * @Input:
   *   stream ::
   *     A pointer to the stream object.
   */
  FT_CALLBACK_DEF( void )
  ft_posix_stream_close( FT_Stream  stream )
  {
    FT_TRACE_BEGIN;
    close( STREAM_PTR_TO_FD( stream ) );

    stream->descriptor.pointer = NULL;
    stream->size               = 0;
    stream->base               = NULL;
    FT_TRACE_END;
  }


  /**************************************************************************
   *
   * @Function:
   *   ft_posix_stream_io
   *
   * @Description:
   *   The function to open a stream.
   *
   * @Input:
   *   stream ::
   *     A pointer to the stream object.
   *
   *   offset ::
   *     The position in the data stream to start reading.
   *
   *   buffer ::
   *     The address of buffer to store the read data.
   *
   *   count ::
   *     The number of bytes to read from the stream.
   *
   * @Return:
   *   The number of bytes actually read.  If `count' is zero (this is,
   *   the function is used for seeking), a non-zero return value
   *   indicates an error.
   */
  FT_CALLBACK_DEF( unsigned long )
  ft_posix_stream_io( FT_Stream       stream,
                      unsigned long   offset,
                      unsigned char*  buffer,
                      unsigned long   count )
  {
    int  file;
    unsigned long total = 0;
    FT_TRACE_BEGIN;

    if ( !count && offset > stream->size )
    {
      FT_TRACE_END;
      return 1;
    }

    file = STREAM_PTR_TO_FD( stream );

    if ( stream->pos != offset )
      lseek( file, (off_t)offset, SEEK_SET );

    if ( count == 0 )
    {
      FT_TRACE_END;
      return 0;
    }

    while ( total < count )
    {
      size_t remain = count - total;
      ssize_t rd = read( file, buffer + total, remain );

      if ( rd <= 0 )
        break;

      total += (unsigned long)rd;
    }

    FT_TRACE_END;
    return total;
  }

#endif

  /* documentation is in ftstream.h */

  FT_BASE_DEF( FT_Error )
  FT_Stream_Open( FT_Stream    stream,
                  const char*  filepathname )
  {
    int  file;


    if ( !stream )
      return FT_THROW( Invalid_Stream_Handle );

    stream->descriptor.pointer = NULL;
    stream->pathname.pointer   = (char*)filepathname;
    stream->base               = NULL;
    stream->pos                = 0;
    stream->read               = NULL;
    stream->close              = NULL;

    FT_TRACE_BEGIN;
    file = open( filepathname, O_RDONLY | O_CLOEXEC );
    FT_TRACE_END;

    if ( file < 0 )
    {
      FT_ERROR(( "FT_Stream_Open:"
                 " could not open `%s'\n", filepathname ));

      return FT_THROW( Cannot_Open_Resource );
    }

    lseek( file, 0, SEEK_END );
    stream->size = (unsigned long)lseek( file, 0, SEEK_CUR );
    if ( !stream->size )
    {
      FT_ERROR(( "FT_Stream_Open:" ));
      FT_ERROR(( " opened `%s' but zero-sized\n", filepathname ));
      close( file );
      return FT_THROW( Cannot_Open_Stream );
    }
    lseek( file, 0, SEEK_SET );

#ifdef CONFIG_LIB_FREETYPE_MBFC
    mbfc_param_t param;
    mbfc_param_init(&param);
    param.fp = FD_TO_PTR( file );
    param.block_size = CONFIG_LIB_FREETYPE_MBFC_BLOCK_SIZE;
    param.cache_num = CONFIG_LIB_FREETYPE_MBFC_CACHE_NUM;
    param.read_cb = mbfc_read_cb;
    param.seek_cb = mbfc_seek_cb;
    mbfc_t *mbfc = mbfc_create(&param);

    stream->descriptor.pointer = mbfc;
    stream->read  = ft_mbfc_stream_io;
    stream->close = ft_mbfc_stream_close;
#else
    stream->descriptor.pointer = FD_TO_PTR( file );
    stream->read  = ft_posix_stream_io;
    stream->close = ft_posix_stream_close;
#endif

    FT_TRACE1(( "FT_Stream_Open:" ));
    FT_TRACE1(( " opened `%s' (%ld bytes) successfully\n",
                filepathname, stream->size ));

    return FT_Err_Ok;
  }

#endif /* !FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT */

#ifdef FT_DEBUG_MEMORY

  extern FT_Int
  ft_mem_debug_init( FT_Memory  memory );

  extern void
  ft_mem_debug_done( FT_Memory  memory );

#endif


  /* documentation is in ftobjs.h */

  FT_BASE_DEF( FT_Memory )
  FT_New_Memory( void )
  {
    FT_Memory  memory;

#if CONFIG_LIB_FREETYPE_MEMSIZE
    struct mm_heap_s* heap;
    FT_MemoryEx* memex;

    const size_t bufsize = CONFIG_LIB_FREETYPE_MEMSIZE * 1024;
    void* buffer = malloc( bufsize );
    DEBUGASSERT( buffer != NULL );

    heap = mm_initialize( "freetype", buffer, bufsize );
    DEBUGASSERT( heap != NULL );

    memex = mm_malloc( heap, sizeof ( FT_MemoryEx ) );
    DEBUGASSERT( memex != NULL );

    memex->buffer = buffer;

    memory = (FT_Memory)memex;

    memory->user = heap;
#else
    memory = (FT_Memory)malloc( sizeof ( *memory ) );
    DEBUGASSERT( memory != NULL );

    memory->user = NULL;
#endif

    memory->alloc   = ft_alloc;
    memory->realloc = ft_realloc;
    memory->free    = ft_free;
#ifdef FT_DEBUG_MEMORY
    ft_mem_debug_init( memory );
#endif

    return memory;
  }


  /* documentation is in ftobjs.h */

  FT_BASE_DEF( void )
  FT_Done_Memory( FT_Memory  memory )
  {
#ifdef FT_DEBUG_MEMORY
    ft_mem_debug_done( memory );
#endif

#if CONFIG_LIB_FREETYPE_MEMSIZE
    FT_MemoryEx* memex = (FT_MemoryEx*)memory;
    struct mm_heap_s* heap = memory->user;
    void* buffer = memex->buffer;

    mm_uninitialize( heap );
    free( buffer );
#else
    free( memory );
#endif
  }


/* END */
