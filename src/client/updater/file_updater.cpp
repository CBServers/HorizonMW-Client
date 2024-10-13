#include <std_include.hpp>

#include "updater.hpp"
#include "file_updater.hpp"
#include "component/console.hpp"

#include <utils/cryptography.hpp>
#include <utils/flags.hpp>
#include <utils/http.hpp>
#include <utils/io.hpp>
#include <utils/concurrency.hpp>
#include <utils/hash.hpp>
#include <utils/string.hpp>

#define UPDATE_SERVER "https://cdn.brad.stream/"
#define UPDATE_SERVER_HOST "https://github.com/CBServers/updater/raw/main/updater/"

#define UPDATE_FILE_MAIN UPDATE_SERVER "h2m.json"
#define UPDATE_FOLDER_MAIN UPDATE_SERVER "h2m/"

#define UPDATE_FILE_HOST UPDATE_SERVER_HOST "h2m.json"
#define UPDATE_FOLDER_HOST UPDATE_SERVER_HOST "h2m/"

#define UPDATE_HOST_BINARY "h2m-mod-cb.exe"

namespace updater
{
	namespace
	{
		std::string get_update_file()
		{
			return UPDATE_FILE_MAIN;
		}

		std::string get_update_folder()
		{
			return UPDATE_FOLDER_MAIN;
		}

		std::string get_update_file_host()
		{
			return UPDATE_FILE_HOST;
		}

		std::string get_update_folder_host()
		{
			return UPDATE_FOLDER_HOST;
		}

		std::string get_filename(const std::filesystem::path path)
		{
			return path.filename().string();
		}

		void throw_error(const std::string error)
		{
			console::error(error.data());
			MSG_BOX_ERROR(error.data());
			utils::nt::terminate();
		}

		std::vector<file_info> parse_file_infos(const std::string& json)
		{
			rapidjson::Document doc{};
			doc.Parse(json.data(), json.size());

			if (!doc.IsArray())
			{
				return {};
			}

			std::vector<file_info> files{};

			for (const auto& element : doc.GetArray())
			{
				if (!element.IsArray())
				{
					continue;
				}

				auto array = element.GetArray();

				file_info info{};
				info.name.assign(array[0].GetString(), array[0].GetStringLength());
				info.size = array[1].GetInt64();
				info.hash.assign(array[2].GetString(), array[2].GetStringLength());

				files.emplace_back(std::move(info));
			}

			return files;
		}

		update_manifest parse_manifest(const std::string& json)
		{
			update_manifest manifest;

			rapidjson::Document doc{};
			doc.Parse(json.data(), json.size());

			if (doc.HasParseError() || !doc.IsObject())
			{
				return {};
			}

			if (doc.HasMember("ManifestHash") && doc["ManifestHash"].IsString())
			{
				manifest.hash = doc["ManifestHash"].GetString();
			}
			else
			{
				return {};
			}

			if (doc.HasMember("files") && doc["files"].IsArray())
			{
				const rapidjson::Value& filesArray = doc["files"];
				for (rapidjson::SizeType i = 0; i < filesArray.Size(); ++i)
				{
					const rapidjson::Value& fileEntry = filesArray[i];
					file_info info;
					info.name = fileEntry[0].GetString();
					info.size = fileEntry[1].GetUint64();
					info.hash = fileEntry[2].GetString();

					manifest.files.push_back(info);
				}
			}
			else
			{
				return {};
			}

			return manifest;
		}

		std::string get_cache_buster()
		{
			return "?" + std::to_string(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count());
		}

		std::vector<file_info> get_file_infos()
		{
			const auto data = utils::http::get_data(get_update_file_host() + get_cache_buster());
			if (!data || !data.has_value())
			{
				return {};
			}

			const auto& result = data.value();
			if (result.code != CURLE_OK)
			{
				return {};
			}

			return parse_file_infos(result.buffer);
		}

		std::string get_hash(const std::string& data)
		{
			return utils::cryptography::sha1::compute(data, true);
		}

		update_manifest get_manifest()
		{
			const auto data = utils::http::get_data(get_update_file() + get_cache_buster());
			if (!data || !data.has_value())
			{
				return {};
			}

			const auto& result = data.value();
			if (result.code != CURLE_OK)
			{
				return {};
			}

			return parse_manifest(result.buffer);
		}

		size_t get_optimal_concurrent_download_count(const size_t file_count)
		{
			size_t cores = std::thread::hardware_concurrency();
			cores = (cores * 2) / 3;
			return std::max(1ull, std::min(cores, file_count));
		}

		bool is_inside_folder(const std::filesystem::path& file, const std::filesystem::path& folder)
		{
			const auto relative = std::filesystem::relative(file, folder);
			const auto start = relative.begin();
			return start != relative.end() && start->string() != "..";
		}
	}

	file_updater::file_updater(std::filesystem::path base,
		std::filesystem::path process_file)
		: base_(std::move(base))
		, process_file_(std::move(process_file))
		, dead_process_file_(process_file_)
	{
		this->dead_process_file_.replace_extension(".exe.old");
		this->delete_old_process_file();
	}

	void file_updater::run() const
	{
		const auto host_files = get_file_infos();
		if (host_files.empty())
		{
			return;
		}

		this->update_host_binary(host_files);

		const auto manifest = get_manifest();
		if (manifest.empty())
		{
			return;
		}
		
		if (!utils::flags::has_flag("verify"))
		{
			if (!this->needs_to_update(manifest.hash))
			{
				return;
			}

			MSG_BOX_INFO("GAME UPDATE REQUIRED!\nPlease wait for update to complete before you can start playing.\nClick OK to continue.");
		}

		this->delete_old_h2m_files(); //do this for those migrating to hmw, will remove eventually 

		const auto outdated_files = this->get_outdated_files(manifest.files);
		if (outdated_files.empty())
		{
			utils::io::write_file(this->get_manifest_file_path(), manifest.hash);
			return;
		}

		const auto update_size = this->get_update_size(outdated_files);
		const auto drive_space = this->get_available_drive_space();
		if (drive_space < update_size)
		{
			double gigabytes = static_cast<double>(update_size) / (1024 * 1024 * 1024);
			throw_error(utils::string::va("Not enough space for update! %.2f GB required.", gigabytes));
		}

		this->update_files(outdated_files);

		utils::io::write_file(this->get_manifest_file_path(), manifest.hash);

		std::this_thread::sleep_for(1s);
	}

	void file_updater::update_file(const file_info& file) const
	{
		const auto url = get_update_folder() + file.name + "?" + file.hash;
		const auto out_file = this->get_drive_filename(file);

		std::string empty{};
		if (!utils::io::write_file(out_file, empty, false))
		{
			throw_error("Failed to write file: " + out_file);
		}

		std::ofstream ofs(out_file, std::ios::binary);
		if (!ofs)
		{
			throw_error("Failed to open file: " + out_file);
		}

		int currentPercent = 0;
		const auto data = utils::http::get_data_stream(url, {}, {}, [&](size_t progress, size_t total_size, size_t speed)
		{
			auto progressRatio = (total_size > 0 && progress >= 0) ? static_cast<double>(progress) / total_size : 0.0;
			auto progressPercent = int(progressRatio * 100.0);
			if (progressPercent == currentPercent)
				return;

			currentPercent = progressPercent;
			console::info("Updating: %s (%d%%)", get_filename(file.name).data(), progressPercent);
		},
		[&](const char* chunk, size_t size)
		{
			if (chunk && size > 0)
			{
				ofs.write(chunk, size);
			}

		});

		ofs.close();

		if (!data || !data.has_value())
		{
			throw_error("Failed to download: " + url);
		}

		const auto& result = data.value();
		if (result.code != CURLE_OK)
		{
			throw_error("Failed to download: " + url);
		}

		if (utils::io::file_size(out_file) != file.size)
		{
			throw_error("Downloaded file size mismatch: " + out_file);
		}

		if (utils::hash::get_file_hash(out_file) != file.hash)
		{
			throw_error("Downloaded file hash mismatch: " + out_file);
		}

	}

	void file_updater::update_host_file(const file_info& file) const
	{
		const auto url = get_update_folder_host() + file.name + "?" + file.hash;

		int currentPercent = 0;
		const auto data = utils::http::get_data(url, {}, {}, [&](const size_t progress, const size_t total, const size_t speed)
		{
			auto progressRatio = (total > 0 && progress >= 0) ? static_cast<double>(progress) / total : 0.0;
			auto progressPercent = int(progressRatio * 100.0);
			if (progressPercent == currentPercent)
				return;

			currentPercent = progressPercent;
			console::info("Updating: %s (%d%%)", file.name.data(), progressPercent);
		});

		if (!data || !data.has_value())
		{
			throw_error("Failed to download: " + url);
		}

		const auto& result = data.value();
		if (result.code != CURLE_OK || result.buffer.size() != file.size || get_hash(result.buffer) != file.hash)
		{
			throw_error("Failed to download: " + url);
		}

		const auto out_file = this->get_drive_filename(file);
		if (!utils::io::write_file(out_file, result.buffer, false))
		{
			throw_error("Failed to write: " + file.name);
		}
	}

	std::vector<file_info> file_updater::get_outdated_files(const std::vector<file_info>& files) const
	{
		const auto thread_count = get_optimal_concurrent_download_count(files.size());
		std::vector<std::thread> threads{};
		std::atomic<size_t> current_index{ 0 };
		std::vector<std::vector<file_info>> per_thread_outdated_files(thread_count);
		utils::concurrency::container<std::exception_ptr> exception{};

		console::info("Verifying files, please wait...");

		for (size_t i = 0; i < thread_count; ++i)
		{
			threads.emplace_back([&, i]()
			{
				auto& local_outdated_files = per_thread_outdated_files[i];

				while (!exception.access<bool>([](const std::exception_ptr& ptr)
				{
					return static_cast<bool>(ptr);
				}))
				{
					const auto index = current_index++;
					if (index >= files.size())
					{
						break;
					}

					try
					{
						const auto& info = files[index];
						if (this->is_outdated_file(info))
						{
							console::error("Verification failed: %s", get_filename(info.name).data());
							local_outdated_files.emplace_back(info);
						}
					}
					catch (...)
					{
						exception.access([](std::exception_ptr& ptr)
						{
							ptr = std::current_exception();
						});
						return;
					}
				}
			});
		}

		for (auto& thread : threads)
		{
			if (thread.joinable())
			{
				thread.join();
			}
		}

		exception.access([](const std::exception_ptr& ptr)
		{
			if (ptr)
			{
				std::rethrow_exception(ptr);
			}
		});

		std::vector<file_info> outdated_files;
		for (const auto& thread_files : per_thread_outdated_files)
		{
			outdated_files.insert(outdated_files.end(), thread_files.begin(), thread_files.end());
		}

		console::info("Finished verifying files");

		return outdated_files;
	}

	const file_info* file_updater::find_outdated_host(const std::vector<file_info>& files) const
	{
#if !defined(NDEBUG)
		if (!utils::flags::has_flag("update"))
		{
			return nullptr;
		}
#endif
		for (const auto& file : files)
		{
			if (file.name != UPDATE_HOST_BINARY)
			{
				continue;
			}

			std::string data{};
			const auto drive_name = this->get_drive_filename(file);
			if (!utils::io::read_file(drive_name, &data))
			{
				return &file;
			}

			if (data.size() != file.size)
			{
				return &file;
			}

			if (get_hash(data) != file.hash)
			{
				return &file;
			}
		}

		return nullptr;
	}

	bool file_updater::needs_to_update(const std::string& hash) const
	{
		const auto manifest_path = this->get_manifest_file_path();
		if (utils::io::file_exists(manifest_path))
		{
			auto manifest_hash = utils::io::read_file(manifest_path);

			if (manifest_hash.empty())
			{
				return true;
			}

			if (manifest_hash == hash)
			{
				return false;
			}
		}

		return true;
	}

	void file_updater::update_host_binary(const std::vector<file_info>& files) const
	{
		const auto* host_file = this->find_outdated_host(files);
		if (!host_file)
		{
			return;
		}
		
		try
		{
			this->move_current_process_file();
			this->update_host_file(*host_file);
		}
		catch (...)
		{
			this->restore_current_process_file();
			throw;
		}

		if (!utils::flags::has_flag("norelaunch"))
		{
			utils::nt::relaunch_self();
		}

		utils::nt::terminate();
	}

	std::size_t file_updater::get_update_size(const std::vector<file_info>& outdated_files) const
	{
		std::size_t total_size = 0;
		for (const auto& file : outdated_files)
		{
			total_size += file.size;
		}

		return total_size;
	}

	std::size_t file_updater::get_available_drive_space() const
	{
		std::filesystem::space_info spaceInfo = std::filesystem::space(this->base_);
		return spaceInfo.available;
	}

	void file_updater::update_files(const std::vector<file_info>& outdated_files) const
	{

		const auto thread_count = get_optimal_concurrent_download_count(outdated_files.size());

		std::vector<std::thread> threads{};
		std::atomic<size_t> current_index{0};

		utils::concurrency::container<std::exception_ptr> exception{};

		console::info("Found outdated files! Downloading/updating files...");

		for (size_t i = 0; i < thread_count; ++i)
		{
			threads.emplace_back([&]()
			{
				while (!exception.access<bool>([](const std::exception_ptr& ptr)
				{
					return static_cast<bool>(ptr);
				}))
				{
					const auto index = current_index++;
					if (index >= outdated_files.size())
					{
						break;
					}

					try
					{
						const auto& file = outdated_files[index];
						this->update_file(file);
					}
					catch (...)
					{
						exception.access([](std::exception_ptr& ptr)
						{
							ptr = std::current_exception();
						});

						return;
					}
				}
			});
		}

		for (auto& thread : threads)
		{
			if (thread.joinable())
			{
				thread.join();
			}
		}

		exception.access([](const std::exception_ptr& ptr)
		{
			if (ptr)
			{
				std::rethrow_exception(ptr);
			}
		});

		console::info("Finished downloading/updating files");
	}

	bool file_updater::is_outdated_file(const file_info& file) const
	{
		console::info("Verifying: %s\n", get_filename(file.name).data());
		const auto drive_name = this->get_drive_filename(file);
		if (!utils::io::file_exists(drive_name))
		{
			return true;
		}

		if (utils::io::file_size(drive_name) != file.size)
		{
			return true;
		}

		const auto hash = utils::hash::get_file_hash(drive_name);
		return hash != file.hash;
	}

	std::string file_updater::get_drive_filename(const file_info& file) const
	{
		if (file.name == UPDATE_HOST_BINARY)
		{
			return (this->process_file_).string();
		}

		return (this->base_ / file.name).string();
	}

	std::string file_updater::get_manifest_file_path() const
	{
		return (this->base_ / "latest.manifest").string();
	}

	void file_updater::move_current_process_file() const
	{
		const auto process_file = this->process_file_.string();
		const auto dead_process_file = this->dead_process_file_.string();
		utils::io::move_file(process_file, dead_process_file);
	}

	void file_updater::restore_current_process_file() const
	{
		const auto dead_process_file = this->dead_process_file_.string();
		const auto process_file = this->process_file_.string();
		utils::io::move_file(dead_process_file, process_file);
	}

	void file_updater::delete_old_process_file() const
	{
		// Wait for other process to die
		const auto dead_process_file = this->dead_process_file_.string();
		for (auto i = 0; i < 4; ++i)
		{
			utils::io::remove_file(dead_process_file);
			if (!utils::io::file_exists(dead_process_file))
			{
				break;
			}

			std::this_thread::sleep_for(2s);
		}
	}

	void file_updater::delete_old_h2m_files() const
	{
		//do this for those migrating to hmw, will remove eventually 
		const auto zone_folder = (this->base_ / "zone").string();
		if (!utils::io::directory_exists(zone_folder))
		{
			return;
		}

		const auto zone_files = utils::io::list_files_recursively(zone_folder);
		for (const auto& file : zone_files)
		{
			if (get_filename(file).contains("h2m_"))
			{
				utils::io::remove_file(file);
			}
		}
	}
}
