#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "updater.hpp"
#include "console.hpp"

#include <utils/flags.hpp>
#include <updater/updater.hpp>
#include <utils/io.hpp>

namespace updater
{
	bool has_mwr()
	{
		if (!utils::io::file_exists("h1_mp64_ship.exe"))
		{
			const auto error = "Can't find a valid h1_mp64_ship.exe.\nMake sure you put h2m-mod-cb.exe in your Modern Warfare Remastered installation folder.";
			console::error(error);
			return false;
		}

		return true;
	}

	void update()
	{
		if (utils::flags::has_flag("noupdate"))
		{
			return;
		}

		try
		{
			if (has_mwr())
			{
				run();
			}
		}
		catch (const std::exception& e)
		{
			console::error(e.what());
		}
		catch (...)
		{

		}
	}

	class component final : public component_interface
	{
	public:
		component()
		{
			this->update_thread_ = std::thread([this]
			{
				update();
			});
		}

		void post_start() override
		{
			join();
		}

		void pre_destroy() override
		{
			join();
		}

		void post_unpack() override
		{
			join();
		}

	private:
		std::thread update_thread_{};

		void join()
		{
			if (this->update_thread_.joinable())
			{
				this->update_thread_.join();
			}
		}
	};
}

REGISTER_COMPONENT(updater::component)
