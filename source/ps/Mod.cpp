/* Copyright (C) 2021 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "ps/Mod.h"

#include <algorithm>

#include "lib/file/file_system.h"
#include "lib/file/vfs/vfs.h"
#include "lib/utf8.h"
#include "ps/Filesystem.h"
#include "ps/GameSetup/GameSetup.h"
#include "ps/GameSetup/Paths.h"
#include "ps/Pyrogenesis.h"
#include "scriptinterface/ScriptInterface.h"

std::vector<CStr> g_modsLoaded;

std::vector<std::vector<CStr>> g_LoadedModVersions;

CmdLineArgs g_args;

JS::Value Mod::GetAvailableMods(const ScriptInterface& scriptInterface)
{
	ScriptRequest rq(scriptInterface);
	JS::RootedObject obj(rq.cx, JS_NewPlainObject(rq.cx));

	const Paths paths(g_args);

	// loop over all possible paths
	OsPath modPath = paths.RData()/"mods";
	OsPath modUserPath = paths.UserData()/"mods";

	DirectoryNames modDirs;
	DirectoryNames modDirsUser;

	GetDirectoryEntries(modPath, NULL, &modDirs);
	// Sort modDirs so that we can do a fast lookup below
	std::sort(modDirs.begin(), modDirs.end());

	PIVFS vfs = CreateVfs();

	for (DirectoryNames::iterator iter = modDirs.begin(); iter != modDirs.end(); ++iter)
	{
		vfs->Clear();
		// Mount with lowest priority, we don't want to overwrite anything
		if (vfs->Mount(L"", modPath / *iter / "", VFS_MOUNT_MUST_EXIST, VFS_MIN_PRIORITY) < 0)
			continue;

		CVFSFile modinfo;
		if (modinfo.Load(vfs, L"mod.json", false) != PSRETURN_OK)
			continue;

		JS::RootedValue json(rq.cx);
		if (!scriptInterface.ParseJSON(modinfo.GetAsString(), &json))
			continue;

		// Valid mod, add it to our structure
		JS_SetProperty(rq.cx, obj, utf8_from_wstring(iter->string()).c_str(), json);
	}

	GetDirectoryEntries(modUserPath, NULL, &modDirsUser);
	bool dev = InDevelopmentCopy();

	for (DirectoryNames::iterator iter = modDirsUser.begin(); iter != modDirsUser.end(); ++iter)
	{
		// If we are in a dev copy we do not mount mods in the user mod folder that
		// are already present in the mod folder, thus we skip those here.
		if (dev && std::binary_search(modDirs.begin(), modDirs.end(), *iter))
			continue;

		vfs->Clear();
		// Mount with lowest priority, we don't want to overwrite anything
		if (vfs->Mount(L"", modUserPath / *iter / "", VFS_MOUNT_MUST_EXIST, VFS_MIN_PRIORITY) < 0)
			continue;

		CVFSFile modinfo;
		if (modinfo.Load(vfs, L"mod.json", false) != PSRETURN_OK)
			continue;

		JS::RootedValue json(rq.cx);
		if (!scriptInterface.ParseJSON(modinfo.GetAsString(), &json))
			continue;

		// Valid mod, add it to our structure
		JS_SetProperty(rq.cx, obj, utf8_from_wstring(iter->string()).c_str(), json);
	}

	return JS::ObjectValue(*obj);
}

void Mod::CacheEnabledModVersions(const shared_ptr<ScriptContext>& scriptContext)
{
	ScriptInterface scriptInterface("Engine", "CacheEnabledModVersions", scriptContext);
	ScriptRequest rq(scriptInterface);

	JS::RootedValue availableMods(rq.cx, GetAvailableMods(scriptInterface));

	g_LoadedModVersions.clear();

	for (const CStr& mod : g_modsLoaded)
	{
		// Ignore mod mod as it is irrelevant for compatibility checks
		if (mod == "mod")
			continue;

		CStr version;
		JS::RootedValue modData(rq.cx);
		if (scriptInterface.GetProperty(availableMods, mod.c_str(), &modData))
			scriptInterface.GetProperty(modData, "version", version);

		g_LoadedModVersions.push_back({mod, version});
	}
}

JS::Value Mod::GetLoadedModsWithVersions(const ScriptInterface& scriptInterface)
{
	ScriptRequest rq(scriptInterface);
	JS::RootedValue returnValue(rq.cx);
	scriptInterface.ToJSVal(rq, &returnValue, g_LoadedModVersions);
	return returnValue;
}

JS::Value Mod::GetEngineInfo(const ScriptInterface& scriptInterface)
{
	ScriptRequest rq(scriptInterface);

	JS::RootedValue mods(rq.cx, Mod::GetLoadedModsWithVersions(scriptInterface));
	JS::RootedValue metainfo(rq.cx);

	ScriptInterface::CreateObject(
		rq,
		&metainfo,
		"engine_version", engine_version,
		"mods", mods);

	scriptInterface.FreezeObject(metainfo, true);

	return metainfo;
}
