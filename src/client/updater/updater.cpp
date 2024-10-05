#include <std_include.hpp>

#include "updater.hpp"
#include "file_updater.hpp"

namespace updater
{
	void run()
	{
		const utils::nt::library host{};
		const auto root_folder = host.get_folder();

		const auto self = utils::nt::library::get_by_address(run);
		const auto self_file = self.get_path();

		const file_updater file_updater{root_folder, self_file};

		file_updater.run();
	}
}
