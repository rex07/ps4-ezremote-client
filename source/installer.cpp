#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <orbis/libkernel.h>
#include <orbis/Bgft.h>
#include <orbis/AppInstUtil.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include <curl/curl.h>
#include <web/request.hpp>
#include <web/urn.hpp>
#include "server/http_server.h"
#include "installer.h"
#include "util.h"
#include "config.h"
#include "windows.h"
#include "lang.h"
#include "system.h"
#include "fs.h"
#include "clients/webdavclient.h"

#define BGFT_HEAP_SIZE (1 * 1024 * 1024)

static OrbisBgftInitParams s_bgft_init_params;

static bool s_bgft_initialized = false;

namespace INSTALLER
{
	int Init(void)
	{
		int ret;

		if (s_bgft_initialized)
		{
			goto done;
		}

		memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));
		{
			s_bgft_init_params.heapSize = BGFT_HEAP_SIZE;
			s_bgft_init_params.heap = (uint8_t *)malloc(s_bgft_init_params.heapSize);
			if (!s_bgft_init_params.heap)
			{
				goto err;
			}
			memset(s_bgft_init_params.heap, 0, s_bgft_init_params.heapSize);
		}

		ret = sceBgftServiceIntInit(&s_bgft_init_params);
		if (ret)
		{
			goto err_bgft_heap_free;
		}

		s_bgft_initialized = true;

	done:
		return 0;

	err_bgft_heap_free:
		if (s_bgft_init_params.heap)
		{
			free(s_bgft_init_params.heap);
			s_bgft_init_params.heap = NULL;
		}

		memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));

	err:
		s_bgft_initialized = false;

		return -1;
	}

	void Exit(void)
	{
		int ret;

		if (!s_bgft_initialized)
		{
			return;
		}

		ret = sceBgftServiceIntTerm();

		if (s_bgft_init_params.heap)
		{
			free(s_bgft_init_params.heap);
			s_bgft_init_params.heap = NULL;
		}

		memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));

		s_bgft_initialized = false;
	}

	std::string getRemoteUrl(const std::string path, bool encodeUrl)
	{
		if (strlen(remote_settings->username) == 0 && strlen(remote_settings->password) == 0 &&
			(remoteclient->clientType() == CLIENT_TYPE_WEBDAV || remoteclient->clientType() == CLIENT_TYPE_HTTP_SERVER))
		{
			std::string full_url = WebDAV::GetHttpUrl(remote_settings->server + path);
			size_t scheme_pos = full_url.find("://");
			if (scheme_pos == std::string::npos)
				return "";
			size_t root_pos = full_url.find("/", scheme_pos + 3);
			std::string host = full_url.substr(0, root_pos);
			std::string path = full_url.substr(root_pos);

			if (encodeUrl)
			{
				Web::Urn::Path uri(path);
				CURL *curl = curl_easy_init();
				path = uri.quote(curl);
				curl_easy_cleanup(curl);
			}

			return host + path;
		}
		else
		{
			std::string encoded_path = path;
			Web::Urn::Path uri(encoded_path);
			CURL *curl = curl_easy_init();
			encoded_path = uri.quote(curl);
			curl_easy_cleanup(curl);
			std::string full_url = std::string("http://localhost:") + std::to_string(http_server_port) + "/rmt_inst" + encoded_path;

			return full_url;
		}

		return "";
	}

	bool canInstallRemotePkg(const std::string &url)
	{
		return true;
	}

	int InstallRemotePkg(const std::string &path, pkg_header *header)
	{
		std::string url = getRemoteUrl(path, true);

		if (url.empty())
			return 0;

		int ret;
		std::string filename = path.substr(path.find_last_of("/")+1);
		std::string cid = std::string((char *)header->pkg_content_id);
		cid = cid.substr(cid.find_first_of("-") + 1, 9);
		int user_id;
		ret = sceUserServiceGetForegroundUser(&user_id);
		const char *package_type;
		uint32_t content_type = BE32(header->pkg_content_type);
		uint32_t flags = BE32(header->pkg_content_flags);
		bool is_patch = false;

		switch (content_type)
		{
		case PKG_CONTENT_TYPE_GD:
			package_type = "PS4GD";
			break;
		case PKG_CONTENT_TYPE_AC:
			package_type = "PS4AC";
			break;
		case PKG_CONTENT_TYPE_AL:
			package_type = "PS4AL";
			break;
		case PKG_CONTENT_TYPE_DP:
			package_type = "PS4DP";
			break;
		default:
			package_type = NULL;
			return 0;
			break;
		}

		if (flags & PKG_CONTENT_FLAGS_FIRST_PATCH ||
			flags & PKG_CONTENT_FLAGS_SUBSEQUENT_PATCH ||
			flags & PKG_CONTENT_FLAGS_DELTA_PATCH ||
			flags & PKG_CONTENT_FLAGS_CUMULATIVE_PATCH)
		{
			is_patch = true;
		}

		OrbisBgftTaskProgress progress_info;
		OrbisBgftDownloadParam params;
		memset(&params, 0, sizeof(params));
		{
			params.userId = user_id;
			params.entitlementType = 5;
			params.id = (char *)header->pkg_content_id;
			params.contentUrl = url.c_str();
			params.contentName = filename.c_str();
			params.iconPath = "";
			params.playgoScenarioId = "0";
			params.option = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM;
			params.packageType = package_type;
			params.packageSubType = "";
			params.packageSize = BE64(header->pkg_size);
		}

	retry:
		int task_id = -1;
		if (!is_patch)
			ret = sceBgftServiceIntDownloadRegisterTask(&params, &task_id);
		else
			ret = sceBgftServiceIntDebugDownloadRegisterPkg(&params, &task_id);
		if (ret == 0x80990088 || ret == 0x80990015)
		{
			sprintf(confirm_message, "%s - %s?", path.c_str(), lang_strings[STR_REINSTALL_CONFIRM_MSG]);
			confirm_state = CONFIRM_WAIT;
			action_to_take = selected_action;
			activity_inprogess = false;
			while (confirm_state == CONFIRM_WAIT)
			{
				sceKernelUsleep(100000);
			}
			activity_inprogess = true;
			selected_action = action_to_take;

			if (confirm_state == CONFIRM_YES)
			{
				ret = sceAppInstUtilAppUnInstall(cid.c_str());
				if (ret != 0)
					goto err;
				goto retry;
			}
		}
		else if (ret > 0)
			goto err;

		ret = sceBgftServiceDownloadStartTask(task_id);
		if (ret)
		{
			goto err;
		}

		Util::Notify("%s queued", filename.c_str());

		file_transfering = true;
		bytes_to_download = 100;
		bytes_transfered = 0;
		while (bytes_transfered < 99)
		{
			memset(&progress_info, 0, sizeof(progress_info));
			ret = sceBgftServiceDownloadGetProgress(task_id, &progress_info);
			if (ret || (progress_info.transferred > 0 && progress_info.errorResult != 0))
				return 0;
			bytes_transfered = (uint32_t)(((float)progress_info.transferred / progress_info.length) * 100.f);
			sceSystemServicePowerTick();
		}

		return 1;
	err:
		return 0;
	}

	int InstallLocalPkg(const std::string &path)
	{
		int ret;
		pkg_header header;
		memset(&header, 0, sizeof(header));
		if (FS::Head(path.c_str(), (void *)&header, sizeof(header)) == 0)
			return 0;

		if (BE32(header.pkg_magic) != PKG_MAGIC)
			return 0;

		char filepath[1024];
		snprintf(filepath, 1023, "%s", path.c_str());
		if (strncmp(path.c_str(), "/data/", 6) == 0)
			snprintf(filepath, 1023, "/user%s", path.c_str());

		char titleId[18];
		memset(titleId, 0, sizeof(titleId));
		int is_app = -1;
		ret = sceAppInstUtilGetTitleIdFromPkg(path.c_str(), titleId, &is_app);
		if (ret)
		{
			return 0;
		}

		OrbisBgftTaskProgress progress_info;
		OrbisBgftDownloadParamEx download_params;
		memset(&download_params, 0, sizeof(download_params));
		{
			download_params.params.entitlementType = 5;
			download_params.params.id = (char *)header.pkg_content_id;
			download_params.params.contentUrl = filepath;
			download_params.params.contentName = path.c_str();
			;
			download_params.params.iconPath = "";
			download_params.params.playgoScenarioId = "0";
			download_params.params.option = ORBIS_BGFT_TASK_OPT_FORCE_UPDATE;
			download_params.slot = 0;
		}

	retry:
		int task_id = -1;
		ret = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&download_params, &task_id);
		if (ret == 0x80990088 || ret == 0x80990015)
		{
			ret = sceAppInstUtilAppUnInstall(titleId);
			if (ret != 0)
				return 0;
			goto retry;
		}
		else if (ret > 0)
			return 0;

		ret = sceBgftServiceDownloadStartTask(task_id);
		if (ret)
			return 0;

		Util::Notify("%s queued", path.c_str());

		file_transfering = true;
		bytes_to_download = 100;
		bytes_transfered = 0;
		while (bytes_transfered < 99)
		{
			memset(&progress_info, 0, sizeof(progress_info));
			ret = sceBgftServiceDownloadGetProgress(task_id, &progress_info);
			if (ret || (progress_info.transferred > 0 && progress_info.errorResult != 0))
				return 0;
			bytes_transfered = (uint32_t)(((float)progress_info.transferred / progress_info.length) * 100.f);
			sceSystemServicePowerTick();
		}
		return 1;

	err:
		return 0;
	}

	int InstallLocalPkg(const std::string &path, pkg_header *header, bool remove_after_install)
	{
		int ret;
		if (strncmp(path.c_str(), "/data/", 6) != 0 &&
			strncmp(path.c_str(), "/user/data/", 11) != 0 &&
			strncmp(path.c_str(), "/mnt/usb", 8) != 0)
			return -1;

		std::string filename = path.substr(path.find_last_of("/")+1);
		char filepath[1024];
		snprintf(filepath, 1023, "%s", path.c_str());
		if (strncmp(path.c_str(), "/data/", 6) == 0)
			snprintf(filepath, 1023, "/user%s", path.c_str());
		char titleId[18];
		memset(titleId, 0, sizeof(titleId));
		int is_app = -1;
		ret = sceAppInstUtilGetTitleIdFromPkg(path.c_str(), titleId, &is_app);
		if (ret)
		{
			return 0;
		}

		OrbisBgftTaskProgress progress_info;
		int prog = 0;
		OrbisBgftDownloadParamEx download_params;
		memset(&download_params, 0, sizeof(download_params));
		{
			download_params.params.entitlementType = 5;
			download_params.params.id = (char *)header->pkg_content_id;
			download_params.params.contentUrl = filepath;
			download_params.params.contentName = filename.c_str();
			;
			download_params.params.iconPath = "";
			download_params.params.playgoScenarioId = "0";
			download_params.params.option = ORBIS_BGFT_TASK_OPT_FORCE_UPDATE;
			download_params.slot = 0;
		}

	retry:
		int task_id = -1;
		ret = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&download_params, &task_id);
		if (ret == 0x80990088 || ret == 0x80990015)
		{
			sprintf(confirm_message, "%s - %s?", path.c_str(), lang_strings[STR_REINSTALL_CONFIRM_MSG]);
			confirm_state = CONFIRM_WAIT;
			action_to_take = selected_action;
			activity_inprogess = false;
			while (confirm_state == CONFIRM_WAIT)
			{
				sceKernelUsleep(100000);
			}
			activity_inprogess = true;
			selected_action = action_to_take;

			if (confirm_state == CONFIRM_YES)
			{
				ret = sceAppInstUtilAppUnInstall(titleId);
				if (ret != 0)
					goto err;
				goto retry;
			}
			else
			{
				if (auto_delete_tmp_pkg)
					FS::Rm(path);
			}
		}
		else if (ret > 0)
			goto err;

		ret = sceBgftServiceDownloadStartTask(task_id);
		if (ret)
		{
			goto err;
		}

		if (!remove_after_install)
		{
			Util::Notify("%s queued", filename.c_str());
			return 1;
		}

		sprintf(activity_message, "%s", lang_strings[STR_WAIT_FOR_INSTALL_MSG]);
		bytes_to_download = 1;
		bytes_transfered = 0;
		while (prog < 99)
		{
			memset(&progress_info, 0, sizeof(progress_info));
			ret = sceBgftServiceDownloadGetProgress(task_id, &progress_info);
			if (ret || (progress_info.transferred > 0 && progress_info.errorResult != 0))
				return -3;
			prog = (uint32_t)(((float)progress_info.transferred / progress_info.length) * 100.f);
			bytes_to_download = progress_info.length;
			bytes_transfered = progress_info.transferred;
		}
		if (auto_delete_tmp_pkg)
			FS::Rm(path);
		return 1;

	err:
		return 0;
	}

	bool ExtractLocalPkg(const std::string &path, const std::string sfo_path, const std::string icon_path)
	{
		pkg_header tmp_hdr;
		FS::Head(path, &tmp_hdr, sizeof(pkg_header));

		size_t entry_count = BE32(tmp_hdr.pkg_entry_count);
		uint32_t entry_table_offset = BE32(tmp_hdr.pkg_table_offset);
		uint64_t entry_table_size = entry_count * sizeof(pkg_table_entry);
		void *entry_table_data = malloc(entry_table_size);

		FILE *fd = FS::OpenRead(path);
		FS::Seek(fd, entry_table_offset);
		FS::Read(fd, entry_table_data, entry_table_size);

		pkg_table_entry *entries = (pkg_table_entry *)entry_table_data;
		void* param_sfo_data = NULL;
		uint32_t param_sfo_offset = 0;
		uint32_t param_sfo_size = 0;
		void *icon0_png_data = NULL;
		uint32_t icon0_png_offset = 0;
		uint32_t icon0_png_size = 0;
		short items = 0;
		for (size_t i = 0; i < entry_count; ++i)
		{
			switch (BE32(entries[i].id))
			{
			case PKG_ENTRY_ID__PARAM_SFO:
				param_sfo_offset = BE32(entries[i].offset);
				param_sfo_size = BE32(entries[i].size);
				items++;
				break;
			case PKG_ENTRY_ID__ICON0_PNG:
				icon0_png_offset = BE32(entries[i].offset);
				icon0_png_size = BE32(entries[i].size);
				items++;
				break;
			default:
				continue;
			}

			if (items == 2)
				break;
		}
		free(entry_table_data);

		if (param_sfo_offset > 0 && param_sfo_size > 0)
		{
			param_sfo_data = malloc(param_sfo_size);
			FILE *out = FS::Create(sfo_path);
			FS::Seek(fd, param_sfo_offset);
			FS::Read(fd, param_sfo_data, param_sfo_size);
			FS::Write(out, param_sfo_data, param_sfo_size);
			FS::Close(out);
			free(param_sfo_data);
		}

		if (icon0_png_offset > 0 && icon0_png_size > 0)
		{
			icon0_png_data = malloc(icon0_png_size);
			FILE *out = FS::Create(icon_path);
			FS::Seek(fd, icon0_png_offset);
			FS::Read(fd, icon0_png_data, icon0_png_size);
			FS::Write(out, icon0_png_data, icon0_png_size);
			FS::Close(out);
			free(icon0_png_data);
		}

		FS::Close(fd);
		return true;
	}

	bool ExtractRemotePkg(const std::string &path, const std::string sfo_path, const std::string icon_path)
	{
		pkg_header tmp_hdr;
		if (!remoteclient->Head(path, &tmp_hdr, sizeof(pkg_header)))
			return false;

		size_t entry_count = BE32(tmp_hdr.pkg_entry_count);
		uint32_t entry_table_offset = BE32(tmp_hdr.pkg_table_offset);
		uint64_t entry_table_size = entry_count * sizeof(pkg_table_entry);
		void *entry_table_data = malloc(entry_table_size);

		if (!remoteclient->GetRange(path, entry_table_data, entry_table_size, entry_table_offset))
			return false;

		pkg_table_entry *entries = (pkg_table_entry *)entry_table_data;
		void* param_sfo_data = NULL;
		uint32_t param_sfo_offset = 0;
		uint32_t param_sfo_size = 0;
		void *icon0_png_data = NULL;
		uint32_t icon0_png_offset = 0;
		uint32_t icon0_png_size = 0;
		short items = 0;
		for (size_t i = 0; i < entry_count; ++i)
		{
			switch (BE32(entries[i].id))
			{
			case PKG_ENTRY_ID__PARAM_SFO:
				param_sfo_offset = BE32(entries[i].offset);
				param_sfo_size = BE32(entries[i].size);
				items++;
				break;
			case PKG_ENTRY_ID__ICON0_PNG:
				icon0_png_offset = BE32(entries[i].offset);
				icon0_png_size = BE32(entries[i].size);
				items++;
				break;
			default:
				continue;
			}

			if (items == 2)
				break;
		}
		free(entry_table_data);

		if (param_sfo_offset > 0 && param_sfo_size > 0)
		{
			param_sfo_data = malloc(param_sfo_size);
			FILE *out = FS::Create(sfo_path);
			if (!remoteclient->GetRange(path, param_sfo_data, param_sfo_size, param_sfo_offset))
			{
				FS::Close(out);
				return false;
			}
			FS::Write(out, param_sfo_data, param_sfo_size);
			FS::Close(out);
			free(param_sfo_data);
		}

		if (icon0_png_offset > 0 && icon0_png_size > 0)
		{
			icon0_png_data = malloc(icon0_png_size);
			FILE *out = FS::Create(icon_path);
			if (!remoteclient->GetRange(path, icon0_png_data, icon0_png_size, icon0_png_offset))
			{
				FS::Close(out);
				return false;
			}
			FS::Write(out, icon0_png_data, icon0_png_size);
			FS::Close(out);
			free(icon0_png_data);
		}

		return true;
	}
}