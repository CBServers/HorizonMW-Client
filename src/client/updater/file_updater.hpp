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

		void update_host_binary(const std::vector<file_info>& outdated_files) const;

		void update_files(const std::vector<file_info>& outdated_files) const;

	private:
		std::filesystem::path base_;
		std::filesystem::path process_file_;
		std::filesystem::path dead_process_file_;

		void update_file(const file_info& file) const;

		[[nodiscard]] bool is_outdated_file(const file_info& file) const;
		[[nodiscard]] std::string get_drive_filename(const file_info& file) const;

		void move_current_process_file() const;
		void restore_current_process_file() const;
		void delete_old_process_file() const;
	};
}
