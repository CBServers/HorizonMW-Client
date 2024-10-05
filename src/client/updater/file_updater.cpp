#include <std_include.hpp>

#include "updater.hpp"
#include "file_updater.hpp"
#include "component/console.hpp"

#include <utils/cryptography.hpp>
#include <utils/flags.hpp>
#include <utils/http.hpp>
#include <utils/io.hpp>
#include <utils/concurrency.hpp>

#define UPDATE_SERVER "https://github.com/CBServers/updater/raw/main/updater/"

#define UPDATE_FILE_MAIN UPDATE_SERVER "h2m.json"
#define UPDATE_FOLDER_MAIN UPDATE_SERVER "h2m/"
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

		std::string get_cache_buster()
		{
			return "?" + std::to_string(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count());
		}

		std::vector<file_info> get_file_infos()
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

			return parse_file_infos(result.buffer);
		}

		std::string get_hash(const std::string& data)
		{
			return utils::cryptography::sha1::compute(data, true);
		}

		const file_info* find_host_file_info(const std::vector<file_info>& outdated_files)
		{
			for (const auto& file : outdated_files)
			{
				if (file.name == UPDATE_HOST_BINARY)
				{
					return &file;
				}
			}

			return nullptr;
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
		const auto files = get_file_infos();
		if (files.empty())
		{
			return;
		}

		const auto outdated_files = this->get_outdated_files(files);
		if (outdated_files.empty())
		{
			return;
		}

		this->update_host_binary(outdated_files);
		this->update_files(outdated_files);

		std::this_thread::sleep_for(1s);
	}

	void file_updater::update_file(const file_info& file) const
	{
		const auto url = get_update_folder() + file.name + "?" + file.hash;

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
			throw std::runtime_error("Failed to download: " + url);
		}

		const auto& result = data.value();
		if (result.code != CURLE_OK || result.buffer.size() != file.size || get_hash(result.buffer) != file.hash)
		{
			throw std::runtime_error("Failed to download: " + url);
		}

		const auto out_file = this->get_drive_filename(file);
		if (!utils::io::write_file(out_file, result.buffer, false))
		{
			throw std::runtime_error("Failed to write: " + file.name);
		}
	}

	std::vector<file_info> file_updater::get_outdated_files(const std::vector<file_info>& files) const
	{
		std::vector<file_info> outdated_files{};

		for (const auto& info : files)
		{
			if (this->is_outdated_file(info))
			{
				outdated_files.emplace_back(info);
			}
		}

		return outdated_files;
	}

	void file_updater::update_host_binary(const std::vector<file_info>& outdated_files) const
	{
		const auto* host_file = find_host_file_info(outdated_files);
		if (!host_file)
		{
			return;
		}

		try
		{
			this->move_current_process_file();
			this->update_files({*host_file});
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

	void file_updater::update_files(const std::vector<file_info>& outdated_files) const
	{

		const auto thread_count = get_optimal_concurrent_download_count(outdated_files.size());

		std::vector<std::thread> threads{};
		std::atomic<size_t> current_index{0};

		utils::concurrency::container<std::exception_ptr> exception{};

		console::info("Downloading/updating files...");

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
#if !defined(NDEBUG)
		if (file.name == UPDATE_HOST_BINARY && !utils::flags::has_flag("update"))
		{
			return false;
		}
#endif

		std::string data{};
		const auto drive_name = this->get_drive_filename(file);
		if (!utils::io::read_file(drive_name, &data))
		{
			return true;
		}

		if (data.size() != file.size)
		{
			return true;
		}

		const auto hash = get_hash(data);
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
}
