#pragma once

namespace App::Feature {
	class GameResourceOverrider {
		struct Implementation;
		const std::shared_ptr<Implementation> m_pImpl;
		static std::weak_ptr<Implementation> s_pImpl;

		static std::shared_ptr<Implementation> AcquireImplementation();

	public:
		GameResourceOverrider();
		~GameResourceOverrider();
	};
}