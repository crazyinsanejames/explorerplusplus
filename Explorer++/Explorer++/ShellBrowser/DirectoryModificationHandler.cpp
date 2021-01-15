// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "ShellBrowser.h"
#include "Config.h"
#include "ItemData.h"
#include "ViewModes.h"
#include "../Helper/ListViewHelper.h"
#include "../Helper/Logging.h"
#include "../Helper/Macros.h"
#include "../Helper/ShellHelper.h"
#include <list>

BOOL g_bNewFileRenamed = FALSE;
int g_iRenamedItem = -1;

void ShellBrowser::DirectoryAltered()
{
	BOOL bNewItemCreated;

	EnterCriticalSection(&m_csDirectoryAltered);

	bNewItemCreated = m_bNewItemCreated;

	SendMessage(m_hListView, WM_SETREDRAW, FALSE, NULL);

	LOG(debug) << _T("ShellBrowser - Starting directory change update for \"")
			   << m_directoryState.directory << _T("\"");

	/* Potential problem:
	After a file is created, it may be renamed shortly afterwards.
	If the rename occurs before the file is added here, the
	addition won't be registered (since technically, the file
	does not exist), and the rename operation will not take place.
	Adding an item that does not exist will corrupt the programs
	state.

	Solution:
	If a file does not exist when adding it, temporarily remember
	its filename. On the next rename operation, if the renamed
	file matches the name of the added file, add the file in-place
	with its new name.
	The operation should NOT be queued, as it is possible that
	other actions for the file will take place before the addition,
	which will again result in an incorrect state.
	*/
	for (const auto &af : m_AlteredList)
	{
		/* Only undertake the modification if the unique folder
		index on the modified item and current folder match up
		(i.e. ensure the directory has not changed since these
		files were modified). */
		if (af.iFolderIndex == m_uniqueFolderId)
		{
			switch (af.dwAction)
			{
			case FILE_ACTION_ADDED:
				LOG(debug) << _T("ShellBrowser - Adding \"") << af.szFileName << _T("\"");
				OnFileAdded(af.szFileName);
				break;

			case FILE_ACTION_MODIFIED:
				LOG(debug) << _T("ShellBrowser - Modifying \"") << af.szFileName << _T("\"");
				OnFileModified(af.szFileName);
				break;

			case FILE_ACTION_REMOVED:
				LOG(debug) << _T("ShellBrowser - Removing \"") << af.szFileName << _T("\"");
				OnFileRemoved(af.szFileName);
				break;

			case FILE_ACTION_RENAMED_OLD_NAME:
				LOG(debug) << _T("ShellBrowser - Old name received \"") << af.szFileName
						   << _T("\"");
				OnFileRenamedOldName(af.szFileName);
				break;

			case FILE_ACTION_RENAMED_NEW_NAME:
				LOG(debug) << _T("ShellBrowser - New name received \"") << af.szFileName
						   << _T("\"");
				OnFileRenamedNewName(af.szFileName);
				break;
			}
		}
	}

	LOG(debug) << _T("ShellBrowser - Finished directory change update for \"")
			   << m_directoryState.directory << _T("\"");

	SendMessage(m_hListView, WM_SETREDRAW, TRUE, NULL);

	/* Ensure the first dropped item is visible. */
	if (m_iDropped != -1)
	{
		if (!ListView_IsItemVisible(m_hListView, m_iDropped))
		{
			ListView_EnsureVisible(m_hListView, m_iDropped, TRUE);
		}

		m_iDropped = -1;
	}

	SendMessage(m_hOwner, WM_USER_DIRECTORYMODIFIED, m_ID, 0);

	if (bNewItemCreated && !m_bNewItemCreated)
	{
		SendMessage(m_hOwner, WM_USER_NEWITEMINSERTED, 0, m_iIndexNewItem);
	}

	m_AlteredList.clear();

	BOOL bFocusSet = FALSE;
	int iIndex;

	/* Select the specified items, and place the
	focus on the first item. */
	auto itr = m_FileSelectionList.begin();
	while (itr != m_FileSelectionList.end())
	{
		iIndex = LocateFileItemIndex(itr->c_str());

		if (iIndex != -1)
		{
			ListViewHelper::SelectItem(m_hListView, iIndex, TRUE);

			if (!bFocusSet)
			{
				ListViewHelper::FocusItem(m_hListView, iIndex, TRUE);
				ListView_EnsureVisible(m_hListView, iIndex, TRUE);

				bFocusSet = TRUE;
			}

			itr = m_FileSelectionList.erase(itr);
		}
		else
		{
			++itr;
		}
	}

	LeaveCriticalSection(&m_csDirectoryAltered);
}

void CALLBACK TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	UNREFERENCED_PARAMETER(uMsg);
	UNREFERENCED_PARAMETER(dwTime);

	KillTimer(hwnd, idEvent);

	SendMessage(hwnd, WM_USER_FILESADDED, idEvent, 0);
}

void ShellBrowser::FilesModified(DWORD Action, const TCHAR *FileName, int EventId, int iFolderIndex)
{
	EnterCriticalSection(&m_csDirectoryAltered);

	SetTimer(m_hOwner, EventId, 200, TimerProc);

	AlteredFile_t af;

	StringCchCopy(af.szFileName, SIZEOF_ARRAY(af.szFileName), FileName);
	af.dwAction = Action;
	af.iFolderIndex = iFolderIndex;

	m_AlteredList.push_back(af);

	LeaveCriticalSection(&m_csDirectoryAltered);
}

void ShellBrowser::OnFileAdded(const TCHAR *szFileName)
{
	IShellFolder *pShellFolder = nullptr;
	PCITEMID_CHILD pidlRelative = nullptr;
	Added_t added;
	TCHAR fullFileName[MAX_PATH];
	BOOL bFileAdded = FALSE;
	HRESULT hr;

	StringCchCopy(fullFileName, SIZEOF_ARRAY(fullFileName), m_directoryState.directory.c_str());
	PathAppend(fullFileName, szFileName);

	unique_pidl_absolute pidlFull;
	hr = SHParseDisplayName(fullFileName, nullptr, wil::out_param(pidlFull), 0, nullptr);

	/* It is possible that by the time a file is registered here,
	it will have already been renamed. In this the following
	check will fail.
	If the file is not added, store its filename. */
	if (SUCCEEDED(hr))
	{
		hr = SHBindToParent(pidlFull.get(), IID_PPV_ARGS(&pShellFolder), &pidlRelative);

		if (SUCCEEDED(hr))
		{
			BOOL bDropped = FALSE;

			if (!m_DroppedFileNameList.empty())
			{
				for (auto itr = m_DroppedFileNameList.begin(); itr != m_DroppedFileNameList.end();
					 itr++)
				{
					if (lstrcmp(szFileName, itr->szFileName) == 0)
					{
						bDropped = TRUE;
						break;
					}
				}
			}

			auto itemId = AddItemInternal(
				pShellFolder, m_directoryState.pidlDirectory.get(), pidlRelative, -1, FALSE);

			/* Only insert the item in its sorted position if it
			wasn't dropped in. */
			if (itemId && m_config->globalFolderSettings.insertSorted && !bDropped)
			{
				// TODO: It would be better to pass the items details to this function directly
				// instead (before the item is added to the awaiting list).
				int sortedPosition = DetermineItemSortedPosition(*itemId);

				auto itr = std::find_if(m_directoryState.awaitingAddList.begin(),
					m_directoryState.awaitingAddList.end(),
					[itemId](const AwaitingAdd_t &awaitingItem) {
						return *itemId == awaitingItem.iItemInternal;
					});

				// The item was added successfully above, so should be in the list of awaiting
				// items.
				assert(itr != m_directoryState.awaitingAddList.end());

				itr->iItem = sortedPosition;
				itr->bPosition = TRUE;
				itr->iAfter = sortedPosition - 1;
			}

			InsertAwaitingItems(m_folderSettings.showInGroups);

			bFileAdded = TRUE;

			pShellFolder->Release();
		}
	}

	if (!bFileAdded)
	{
		/* The file does not exist. However, it is possible
		that is was simply renamed shortly after been created.
		Record the filename temporarily (so that it can later
		be added). */
		StringCchCopy(added.szFileName, SIZEOF_ARRAY(added.szFileName), szFileName);
		m_FilesAdded.push_back(added);
	}
}

void ShellBrowser::OnFileRemoved(const TCHAR *szFileName)
{
	std::list<Added_t>::iterator itr;
	int iItemInternal;
	BOOL bFound = FALSE;

	/* First check if this item is in the queue of awaiting
	items. If it is, remove it. */
	for (itr = m_FilesAdded.begin(); itr != m_FilesAdded.end(); itr++)
	{
		if (lstrcmp(szFileName, itr->szFileName) == 0)
		{
			m_FilesAdded.erase(itr);
			bFound = TRUE;
			break;
		}
	}

	if (!bFound)
	{
		iItemInternal = LocateFileItemInternalIndex(szFileName);

		if (iItemInternal != -1)
		{
			RemoveItem(iItemInternal);
		}
	}
}

/*
 * Modifies the attributes of an item currently in the listview.
 */
void ShellBrowser::OnFileModified(const TCHAR *FileName)
{
	HANDLE hFirstFile;
	ULARGE_INTEGER ulFileSize;
	LVITEM lvItem;
	TCHAR fullFileName[MAX_PATH];
	BOOL bFolder;
	BOOL res;
	int iItem;
	int iItemInternal = -1;

	iItem = LocateFileItemIndex(FileName);

	/* Although an item may not have been added to the listview
	yet, it is critical that its' size still be updated if
	necessary.
	It is possible (and quite likely) that the file add and
	modified messages will be sent in the same group, meaning
	that when the modification message is processed, the item
	is not in the listview, but it still needs to be updated.
	Therefore, instead of searching for items solely in the
	listview, also look through the list of pending file
	additions. */

	if (iItem == -1)
	{
		/* The item doesn't exist in the listview. This can
		happen when a file has been created with a non-zero
		size, but an item has not yet been inserted into
		the listview.
		Search through the list of items waiting to be
		inserted, so that files the have just been created
		can be updated without them residing within the
		listview. */
		for (const auto &awaitingItem : m_directoryState.awaitingAddList)
		{
			if (lstrcmp(m_itemInfoMap.at(awaitingItem.iItemInternal).wfd.cFileName, FileName) == 0)
			{
				iItemInternal = awaitingItem.iItemInternal;
				break;
			}
		}
	}
	else
	{
		/* The item exists in the listview. Determine its
		internal index from its listview information. */
		lvItem.mask = LVIF_PARAM;
		lvItem.iItem = iItem;
		lvItem.iSubItem = 0;
		res = ListView_GetItem(m_hListView, &lvItem);

		if (res != FALSE)
		{
			iItemInternal = (int) lvItem.lParam;
		}

		TCHAR szFullFileName[MAX_PATH];
		StringCchCopy(
			szFullFileName, SIZEOF_ARRAY(szFullFileName), m_directoryState.directory.c_str());
		PathAppend(szFullFileName, FileName);

		/* When a file is modified, its icon overlay may change.
		This is the case when modifying a file managed by
		TortoiseSVN, for example. */
		SHFILEINFO shfi;
		DWORD_PTR dwRes = SHGetFileInfo(
			szFullFileName, 0, &shfi, sizeof(SHFILEINFO), SHGFI_ICON | SHGFI_OVERLAYINDEX);

		if (dwRes != 0)
		{
			lvItem.mask = LVIF_STATE;
			lvItem.iItem = iItem;
			lvItem.iSubItem = 0;
			lvItem.stateMask = LVIS_OVERLAYMASK;
			lvItem.state = INDEXTOOVERLAYMASK(shfi.iIcon >> 24);
			ListView_SetItem(m_hListView, &lvItem);

			DestroyIcon(shfi.hIcon);
		}
	}

	if (iItemInternal != -1)
	{
		/* Is this item a folder? */
		bFolder = (m_itemInfoMap.at(iItemInternal).wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			== FILE_ATTRIBUTE_DIRECTORY;

		ulFileSize.LowPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeLow;
		ulFileSize.HighPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeHigh;

		m_directoryState.totalDirSize.QuadPart -= ulFileSize.QuadPart;

		if (ListView_GetItemState(m_hListView, iItem, LVIS_SELECTED) == LVIS_SELECTED)
		{
			ulFileSize.LowPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeLow;
			ulFileSize.HighPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeHigh;

			m_directoryState.fileSelectionSize.QuadPart -= ulFileSize.QuadPart;
		}

		StringCchCopy(fullFileName, SIZEOF_ARRAY(fullFileName), m_directoryState.directory.c_str());
		PathAppend(fullFileName, FileName);

		hFirstFile = FindFirstFile(fullFileName, &m_itemInfoMap.at(iItemInternal).wfd);

		if (hFirstFile != INVALID_HANDLE_VALUE)
		{
			ulFileSize.LowPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeLow;
			ulFileSize.HighPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeHigh;

			m_directoryState.totalDirSize.QuadPart += ulFileSize.QuadPart;

			if (ListView_GetItemState(m_hListView, iItem, LVIS_SELECTED) == LVIS_SELECTED)
			{
				ulFileSize.LowPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeLow;
				ulFileSize.HighPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeHigh;

				m_directoryState.fileSelectionSize.QuadPart += ulFileSize.QuadPart;
			}

			if ((m_itemInfoMap.at(iItemInternal).wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
				== FILE_ATTRIBUTE_HIDDEN)
			{
				ListView_SetItemState(m_hListView, iItem, LVIS_CUT, LVIS_CUT);
			}
			else
				ListView_SetItemState(m_hListView, iItem, 0, LVIS_CUT);

			if (m_folderSettings.viewMode == +ViewMode::Details)
			{
				if (m_pActiveColumns != nullptr)
				{
					for (auto itrColumn = m_pActiveColumns->begin();
						 itrColumn != m_pActiveColumns->end(); itrColumn++)
					{
						if (itrColumn->bChecked)
						{
							QueueColumnTask(iItemInternal, itrColumn->type);
						}
					}
				}
			}

			FindClose(hFirstFile);

			if (m_folderSettings.showInGroups)
			{
				int groupId = DetermineItemGroup(iItemInternal);
				InsertItemIntoGroup(iItem, groupId);
			}
		}
		else
		{
			/* The file may not exist if, for example, it was
			renamed just after a file with the same name was
			deleted. If this does happen, a modification
			message will likely be sent out after the file
			has been renamed, indicating the new items properties.
			However, the files' size will be subtracted on
			modification. If the internal structures still hold
			the old size, the total directory size will become
			corrupted. */
			m_itemInfoMap.at(iItemInternal).wfd.nFileSizeLow = 0;
			m_itemInfoMap.at(iItemInternal).wfd.nFileSizeHigh = 0;
		}
	}
}

void ShellBrowser::OnFileRenamedOldName(const TCHAR *szFileName)
{
	/* Loop through each file that is awaiting add to check for the
	renamed file. */
	for (auto itr = m_FilesAdded.begin(); itr != m_FilesAdded.end(); itr++)
	{
		if (lstrcmp(szFileName, itr->szFileName) == 0)
		{
			g_bNewFileRenamed = TRUE;
			m_FilesAdded.erase(itr);
			return;
		}
	}

	TCHAR fullFileName[MAX_PATH];
	HRESULT hr =
		StringCchCopy(fullFileName, SIZEOF_ARRAY(fullFileName), m_directoryState.directory.c_str());

	if (FAILED(hr))
	{
		return;
	}

	BOOL res = PathAppend(fullFileName, szFileName);

	if (!res)
	{
		return;
	}

	unique_pidl_absolute pidl;
	hr = CreateSimplePidl(fullFileName, wil::out_param(pidl));

	if (FAILED(hr))
	{
		return;
	}

	/* Find the index of the item that was renamed...
	Store the index so that it is known which item needs
	renaming when the files new name is received. */
	auto internalIndex = GetItemInternalIndexForPidl(pidl.get());

	if (internalIndex)
	{
		g_iRenamedItem = *internalIndex;
	}
}

void ShellBrowser::OnFileRenamedNewName(const TCHAR *szFileName)
{
	if (g_bNewFileRenamed)
	{
		/* The file that was previously added was renamed before
		it could be added. Add the file now. */
		OnFileAdded(szFileName);

		g_bNewFileRenamed = FALSE;
	}
	else
	{
		RenameItem(g_iRenamedItem, szFileName);
	}
}

void ShellBrowser::RenameItem(int internalIndex, const TCHAR *szNewFileName)
{
	if (internalIndex == -1)
	{
		return;
	}

	TCHAR szFullFileName[MAX_PATH];
	StringCchCopy(szFullFileName, SIZEOF_ARRAY(szFullFileName), m_directoryState.directory.c_str());
	PathAppend(szFullFileName, szNewFileName);

	unique_pidl_absolute pidlFull;
	HRESULT hr = SHParseDisplayName(szFullFileName, nullptr, wil::out_param(pidlFull), 0, nullptr);

	if (FAILED(hr))
	{
		return;
	}

	wil::com_ptr_nothrow<IShellFolder> shellFolder;
	PCITEMID_CHILD pidlChild = nullptr;
	hr = SHBindToParent(pidlFull.get(), IID_PPV_ARGS(&shellFolder), &pidlChild);

	if (FAILED(hr))
	{
		return;
	}

	auto itemInfo =
		GetItemInformation(shellFolder.get(), m_directoryState.pidlDirectory.get(), pidlChild);

	if (!itemInfo)
	{
		return;
	}

	m_itemInfoMap[internalIndex] = std::move(*itemInfo);
	const ItemInfo_t &updatedItemInfo = m_itemInfoMap[internalIndex];

	auto itemIndex = LocateItemByInternalIndex(internalIndex);

	// Items may be filtered out of the listview, so it's valid for an item not to be found.
	if (!itemIndex)
	{
		if (!IsFileFiltered(updatedItemInfo))
		{
			UnfilterItem(internalIndex);
		}

		return;
	}

	if (IsFileFiltered(updatedItemInfo))
	{
		RemoveFilteredItem(*itemIndex, internalIndex);
		return;
	}

	InvalidateIconForItem(*itemIndex);

	if (m_folderSettings.viewMode == +ViewMode::Details)
	{
		// Although only the item name has changed, other columns might need to be updated as well
		// (e.g. type, extension, 8.3 name). Therefore, all columns will be invalidated here.
		// Note that this is more efficient than simply queuing tasks to set the text for each
		// column, since that won't be necessary if the item isn't currently visible.
		InvalidateAllColumnsForItem(*itemIndex);
	}
	else
	{
		BasicItemInfo_t basicItemInfo = getBasicItemInfo(internalIndex);
		std::wstring filename = ProcessItemFileName(basicItemInfo, m_config->globalFolderSettings);
		ListView_SetItemText(m_hListView, *itemIndex, 0, filename.data());
	}

	ListView_SortItems(m_hListView, SortStub, this);

	if (m_folderSettings.showInGroups)
	{
		int groupId = DetermineItemGroup(internalIndex);
		InsertItemIntoGroup(*itemIndex, groupId);
	}
}

void ShellBrowser::InvalidateAllColumnsForItem(int itemIndex)
{
	if (m_folderSettings.viewMode != +ViewMode::Details)
	{
		return;
	}

	auto numColumns = std::count_if(
		m_pActiveColumns->begin(), m_pActiveColumns->end(), [](const Column_t &column) {
			return column.bChecked;
		});

	for (int i = 0; i < numColumns; i++)
	{
		ListView_SetItemText(m_hListView, itemIndex, i, LPSTR_TEXTCALLBACK);
	}
}

void ShellBrowser::InvalidateIconForItem(int itemIndex)
{
	LVITEM lvItem;
	lvItem.mask = LVIF_IMAGE;
	lvItem.iItem = itemIndex;
	lvItem.iSubItem = 0;
	lvItem.iImage = I_IMAGECALLBACK;
	ListView_SetItem(m_hListView, &lvItem);
}