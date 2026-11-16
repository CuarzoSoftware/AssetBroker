#ifndef CZ_ABLOG_H
#define CZ_ABLOG_H

#include <CZ/Core/CZLogger.h>

/// AssetBroker's logger. Verbosity: CZ_ASSET_BROKER_LOG_LEVEL.
#define ABLog ABLogger()

const CZ::CZLogger &ABLogger() noexcept;

#endif // CZ_ABLOG_H
