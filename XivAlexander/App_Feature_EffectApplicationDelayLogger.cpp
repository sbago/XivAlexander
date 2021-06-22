#include "pch.h"
#include "App_Feature_EffectApplicationDelayLogger.h"
#include "App_Network_SocketHook.h"
#include "App_Network_Structures.h"

class App::Feature::EffectApplicationDelayLogger::Internals {
public:

	class SingleConnectionHandler {
	public:
		Internals& internals;
		Network::SingleConnection& conn;
		SingleConnectionHandler(Internals& internals, Network::SingleConnection& conn)
			: internals(internals)
			, conn(conn) {
			using namespace Network::Structures;

			const auto& config = Config::Instance().Game;

			conn.AddIncomingFFXIVMessageHandler(this, [&](FFXIVMessage* pMessage, std::vector<uint8_t>& additionalMessages) {
				if (pMessage->Type == SegmentType::IPC && pMessage->Data.IPC.Type == IpcType::InterestedType) {
					if (config.S2C_ActionEffects[0] == pMessage->Data.IPC.SubType
						|| config.S2C_ActionEffects[1] == pMessage->Data.IPC.SubType
						|| config.S2C_ActionEffects[2] == pMessage->Data.IPC.SubType
						|| config.S2C_ActionEffects[3] == pMessage->Data.IPC.SubType
						|| config.S2C_ActionEffects[4] == pMessage->Data.IPC.SubType) {

						const auto& actionEffect = pMessage->Data.IPC.Data.S2C_ActionEffect;
						Misc::Logger::GetLogger().Format(
							LogCategory::EffectApplicationDelayLogger,
							"{:x}: S2C_ActionEffect({:04x}): actionId={:04x} sourceSequence={:04x} wait={:d}ms",
							conn.GetSocket(),
							pMessage->Data.IPC.SubType,
							actionEffect.ActionId,
							actionEffect.SourceSequence,
							static_cast<int>(1000 * actionEffect.AnimationLockDuration));

					}
					else if (pMessage->Data.IPC.SubType == config.S2C_AddStatusEffect) {
						const auto& addStatusEffect = pMessage->Data.IPC.Data.S2C_AddStatusEffect;
						std::string effects;
						for (int i = 0; i < addStatusEffect.EffectCount; ++i) {
							const auto& entry = addStatusEffect.Effects[i];
							effects += std::format(
								"\n\teffectId={:04x} duration={:.3f} sourceActorId={:08x}",
								entry.EffectId,
								entry.Duration,
								entry.SourceActorId
							);
						}
						Misc::Logger::GetLogger().Format(
							LogCategory::EffectApplicationDelayLogger,
							"{:x}: S2C_AddStatusEffect: relatedActionSequence={:08x} actorId={:08x} HP={:d}/{:d} MP={:d} shield={:d}{}",
							conn.GetSocket(),
							addStatusEffect.RelatedActionSequence,
							addStatusEffect.ActorId,
							addStatusEffect.CurrentHp,
							addStatusEffect.MaxHp,
							addStatusEffect.CurentMp,
							addStatusEffect.DamageShield,
							effects
						);
					}
				}
				return true;
				});
		}
		~SingleConnectionHandler() {
			conn.RemoveMessageHandlers(this);
		}
	};

	std::map<Network::SingleConnection*, std::unique_ptr<SingleConnectionHandler>> m_handlers;

	Internals() {
		Network::SocketHook::Instance()->AddOnSocketFoundListener(this, [&](Network::SingleConnection& conn) {
			m_handlers.emplace(&conn, std::make_unique<SingleConnectionHandler>(*this, conn));
			});
		Network::SocketHook::Instance()->AddOnSocketGoneListener(this, [&](Network::SingleConnection& conn) {
			m_handlers.erase(&conn);
			});
	}

	~Internals() {
		m_handlers.clear();
		Network::SocketHook::Instance()->RemoveListeners(this);
	}
};

App::Feature::EffectApplicationDelayLogger::EffectApplicationDelayLogger()
	: impl(std::make_unique<Internals>()) {
}

App::Feature::EffectApplicationDelayLogger::~EffectApplicationDelayLogger() = default;
