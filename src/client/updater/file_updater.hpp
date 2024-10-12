#pragma once

#include "file_info.hpp"

namespace updater
{
	class file_updater
	{
	public:
		file_updater(std::filesystem::path base, std::filesystem::path process_file);

		void run() const;

		[[nodiscard]] std::vector<file_info> get_outdated_files(const std::vector<file_info>& files) const;
		[[nodiscard]] const file_info* find_outdated_host(const std::vector<file_info>& outdated_files) const;

		void update_host_binary(const std::vector<file_info>& outdated_files) const;

		void update_files(const std::vector<file_info>& outdated_files) const;
		bool needs_to_update(const std::string& hash) const;

	private:
		std::filesystem::path base_;
		std::filesystem::path process_file_;
		std::filesystem::path dead_process_file_;

		void update_file(const file_info& file) const;
		void update_host_file(const file_info& file) const;

		std::size_t get_update_size(const std::vector<file_info>& files) const;
		std::size_t get_available_drive_space() const;

		
		[[nodiscard]] bool is_outdated_file(const file_info& file) const;
		[[nodiscard]] std::string get_drive_filename(const file_info& file) const;
		[[nodiscard]] std::string get_manifest_file_path() const;

		void move_current_process_file() const;
		void restore_current_process_file() const;
		void delete_old_process_file() const;
	};
}
