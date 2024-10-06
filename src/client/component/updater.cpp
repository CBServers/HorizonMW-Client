#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "updater.hpp"

#include <utils/flags.hpp>
#include <updater/updater.hpp>

namespace updater
{
	void update()
	{
		if (utils::flags::has_flag("noupdate"))
		{
			return;
		}

		try
		{
			run();
		}
		catch (...)
		{
			utils::nt::terminate();
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
