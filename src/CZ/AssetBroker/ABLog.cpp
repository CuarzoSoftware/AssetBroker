#include <CZ/AssetBroker/ABLog.h>

using namespace CZ;

const CZ::CZLogger &ABLogger() noexcept
{
    static CZLogger logger { "AssetBroker", "CZ_ASSET_BROKER_LOG_LEVEL" };
    return logger;
}
