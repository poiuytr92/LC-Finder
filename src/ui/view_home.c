﻿/* ***************************************************************************
* view_home.c -- home view
*
* Copyright (C) 2016 by Liu Chao <lc-soft@live.cn>
*
* This file is part of the LC-Finder project, and may only be used, modified,
* and distributed under the terms of the GPLv2.
*
* By continuing to use, modify, or distribute this file you indicate that you
* have read the license and understand and accept it fully.
*
* The LC-Finder project is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
* or FITNESS FOR A PARTICULAR PURPOSE. See the GPL v2 for more details.
*
* You should have received a copy of the GPLv2 along with this file. It is
* usually in the LICENSE.TXT file, If not, see <http://www.gnu.org/licenses/>.
* ****************************************************************************/

/* ****************************************************************************
* view_home.c -- 主页集锦视图
*
* 版权所有 (C) 2016 归属于 刘超 <lc-soft@live.cn>
*
* 这个文件是 LC-Finder 项目的一部分，并且只可以根据GPLv2许可协议来使用、更改和
* 发布。
*
* 继续使用、修改或发布本文件，表明您已经阅读并完全理解和接受这个许可协议。
*
* LC-Finder 项目是基于使用目的而加以散布的，但不负任何担保责任，甚至没有适销
* 性或特定用途的隐含担保，详情请参照GPLv2许可协议。
*
* 您应已收到附随于本文件的GPLv2许可协议的副本，它通常在 LICENSE 文件中，如果
* 没有，请查看：<http://www.gnu.org/licenses/>.
* ****************************************************************************/

#include <time.h>
#include <stdio.h>
#include <string.h>
#include "finder.h"
#include <LCUI/timer.h>
#include <LCUI/display.h>
#include <LCUI/gui/widget.h>
#include <LCUI/gui/widget/textview.h>
#include "thumbview.h"

#define TEXT_TIME_TITLE		L"%d年%d月"
#define TEXT_TIME_SUBTITLE	L"%d月%d日 %d张照片"
#define TEXT_TIME_SUBTITLE2	L"%d月%d日 - %d月%d日 %d张照片"

/** 时间分割线功能的数据 */
typedef struct TimeSeparatorRec_ {
	int files;		/**< 当前时间段内的文件总数 */
	struct tm time;		/**< 当前时间段的起始时间 */
	LCUI_Widget subtitle;	/**< 子标题 */
} TimeSeparatorRec, *TimeSeparator;

/** 文件扫描功能的相关数据 */
typedef struct FileScannerRec_ {
	LCUI_Thread tid;
	LCUI_Cond cond;
	LCUI_BOOL is_running;
	LinkedList *cache;
	LinkedList files_cache[2];
} FileScannerRec, *FileScanner;

/** 视图同步功能的相关数据 */
typedef struct ViewSyncRec_ {
	LCUI_Thread tid;
	LCUI_BOOL is_running;
	LCUI_Mutex mutex;
} ViewSyncRec, *ViewSync;

/** 主页集锦视图的相关数据 */
static struct HomeCollectionViewData {
	LCUI_Widget view;
	LCUI_Widget items;
	LCUI_Widget info_path;
	LCUI_Widget tip_empty;
	LinkedList files;
	ViewSyncRec viewsync;
	FileScannerRec scanner;
	TimeSeparatorRec separator;
} this_view;

static void OnBtnSyncClick( LCUI_Widget w, LCUI_WidgetEvent e, void *arg )
{
	LCFinder_TriggerEvent( EVENT_SYNC, NULL );
}

static void OnItemClick( LCUI_Widget w, LCUI_WidgetEvent e, void *arg )
{
	DB_File f = e->data;
	_DEBUG_MSG( "open file: %s\n", f->path );
}

static void OnDeleteDBFile( void *arg )
{
	DB_File f = arg;
	free( f->path );
	free( f );
}

/** 扫描全部文件 */
static int FileScanner_ScanAll( FileScanner scanner )
{
	DB_File file;
	DB_Query query;
	int i, total, count;
	DB_QueryTermsRec terms;
	terms.dirpath = NULL;
	terms.n_dirs = 0;
	terms.n_tags = 0;
	terms.limit = 100;
	terms.offset = 0;
	terms.score = NONE;
	terms.tags = NULL;
	terms.dirs = NULL;
	terms.create_time = DESC;
	query = DB_NewQuery( &terms );
	count = total = DBQuery_GetTotalFiles( query );
	while( scanner->is_running && count > 0 ) {
		DB_DeleteQuery( query );
		query = DB_NewQuery( &terms );
		i = count < terms.limit ? count : terms.limit;
		for( ; scanner->is_running && i > 0; --i ) {
			file = DBQuery_FetchFile( query );
			if( !file ) {
				break;
			}
			LinkedList_Append( scanner->cache, file );
		}
		count -= terms.limit;
		terms.offset += terms.limit;
		LCUICond_Signal( &scanner->cond );
	}
	LCUICond_Signal( &scanner->cond );
	return total;
}

/** 初始化文件扫描 */
static void FileScanner_Init( FileScanner scanner )
{
	LCUICond_Init( &scanner->cond );
	LinkedList_Init( &scanner->files_cache[0] );
	LinkedList_Init( &scanner->files_cache[1] );
	this_view.scanner.cache = &scanner->files_cache[0];
}

/** 重置文件扫描 */
static void FileScanner_Reset( FileScanner scanner )
{
	if( this_view.scanner.is_running ) {
		this_view.scanner.is_running = FALSE;
		LCUIThread_Join( this_view.scanner.tid, NULL );
	}
	LinkedList_Clear( &scanner->files_cache[0], OnDeleteDBFile );
	LinkedList_Clear( &scanner->files_cache[1], OnDeleteDBFile );
	this_view.scanner.cache = &scanner->files_cache[0];
}

/** 文件扫描线程 */
static void FileScanner_Thread( void *arg )
{
	int count;
	this_view.scanner.is_running = TRUE;
	count = FileScanner_ScanAll( &this_view.scanner );
	if( count > 0 ) {
		Widget_AddClass( this_view.tip_empty, "hide" );
		Widget_Hide( this_view.tip_empty );
	} else {
		Widget_RemoveClass( this_view.tip_empty, "hide" );
		Widget_Show( this_view.tip_empty );
	}
	this_view.scanner.is_running = FALSE;
	LCUIThread_Exit( NULL );
}

/** 开始扫描 */
static void FileScanner_Start( FileScanner scanner )
{
	LCUIThread_Create( &scanner->tid, FileScanner_Thread, NULL );
}

/** 获取已扫描的文件列表 */
static LinkedList *FileScanner_GetFiles( FileScanner scanner )
{
	LinkedList *list = scanner->cache;
	if( scanner->cache == scanner->files_cache ) {
		scanner->cache = &scanner->files_cache[1];
	} else {
		scanner->cache = scanner->files_cache;
	}
	return list;
}

/** 载入列表项 */
static void ViewSync_LoadItems( LinkedList *list )
{
	time_t time;
	struct tm *t;
	wchar_t text[128];
	LCUI_Widget item;
	LinkedListNode *node, *prev_node;
	TimeSeparator ts = &this_view.separator;
	LinkedList_ForEach( node, list ) {
		DB_File file = node->data;
		prev_node = node->prev;
		LinkedList_Unlink( list, node );
		LinkedList_AppendNode( &this_view.files, node );
		node = prev_node;
		time = file->create_time;
		t = localtime( &time );
		/* 如果当前文件的创建时间超出当前时间段，则新建分割线 */
		if( t->tm_year != ts->time.tm_year ||
		    t->tm_mon != ts->time.tm_mon ) {
			LCUI_Widget title = LCUIWidget_New( "textview" );
			/** 如果是第一个时间段，则取消顶部的边距 */
			if( ts->files == 0 ) {
				SetStyle( title->custom_style, 
					  key_margin_top, 0, px );
			}
			ts->subtitle = LCUIWidget_New( "textview" );
			Widget_AddClass( ts->subtitle, 
					 "time-separator-subtitle" );
			Widget_AddClass( title, "time-separator-title" );
			/* 设置时间段的标题 */
			swprintf( text, 128, TEXT_TIME_TITLE, 
				  1900 + t->tm_year, t->tm_mon + 1 );
			TextView_SetTextW( title, text );
			ThumbView_Append( this_view.items, title );
			ThumbView_Append( this_view.items, ts->subtitle );
			ts->files = 0;
			ts->time = *t;
		}
		item = ThumbView_AppendPicture( this_view.items, file->path );
		if( item ) {
			ts->files += 1;
			Widget_BindEvent( item, "click", OnItemClick,
					  file, NULL );
		}
		/** 如果时间跨度不超过一天 */
		if( t->tm_year == ts->time.tm_year && 
		    t->tm_mon == ts->time.tm_mon &&
		    t->tm_mday == ts->time.tm_mday ) {
			swprintf( text, 128, TEXT_TIME_SUBTITLE, 
				  t->tm_mon + 1, t->tm_mday, ts->files );
		} else {
			swprintf( text, 128, TEXT_TIME_SUBTITLE2, 
				  t->tm_mon + 1, t->tm_mday, 
				  ts->time.tm_mon + 1, ts->time.tm_mday,
				  ts->files );
		}
		TextView_SetTextW( ts->subtitle, text );
	}
}

/** 视图同步线程 */
static void ViewSync_Thread( void *arg )
{
	ViewSync vs;
	LinkedList *list;
	vs = &this_view.viewsync;
	vs->is_running = TRUE;
	while( this_view.viewsync.is_running ) {
		LCUICond_Wait( &this_view.scanner.cond, &vs->mutex );
		list = FileScanner_GetFiles( &this_view.scanner );
		if( list->length > 0 ) {
			ViewSync_LoadItems( list );
		}
		LCUIMutex_Unlock( &vs->mutex );
	}
}

/** 载入集锦中的文件列表 */
static void LoadCollectionFiles( void )
{
	FileScanner_Reset( &this_view.scanner );
	LCUIMutex_Lock( &this_view.viewsync.mutex );
	this_view.separator.files = 0;
	memset( &this_view.separator.time, 0, sizeof(TimeSeparatorRec) );
	ThumbView_Lock( this_view.items );
	ThumbView_Empty( this_view.items );
	LinkedList_ClearData( &this_view.files, OnDeleteDBFile );
	FileScanner_Start( &this_view.scanner );
	ThumbView_Unlock( this_view.items );
	LCUIMutex_Unlock( &this_view.viewsync.mutex );
}

static void OnSyncDone( LCUI_Event e, void *arg )
{
	LoadCollectionFiles();
}

void UI_InitHomeView( void )
{
	LCUI_Widget btn;
	LinkedList_Init( &this_view.files );
	btn = LCUIWidget_GetById( "btn-sync-collection-files" );
	this_view.view = LCUIWidget_GetById( "view-home" );
	this_view.items = LCUIWidget_GetById( "home-collection-list" );
	this_view.tip_empty = LCUIWidget_GetById( "tip-empty-collection" );
	Widget_BindEvent( btn, "click", OnBtnSyncClick, NULL, NULL );
	LCUIThread_Create( &this_view.viewsync.tid, ViewSync_Thread, NULL );
	FileScanner_Init( &this_view.scanner );
	LCUIMutex_Init( &this_view.viewsync.mutex );
	LCFinder_BindEvent( EVENT_SYNC_DONE, OnSyncDone, NULL );
	LoadCollectionFiles();
}

void UI_ExitHomeView( void )
{
	FileScanner_Reset( &this_view.scanner );
	if( this_view.viewsync.is_running ) {
		this_view.viewsync.is_running = FALSE;
		LCUIThread_Join( this_view.viewsync.tid, NULL );
	}
}
