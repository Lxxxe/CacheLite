#include "cachelite/database/KeyValueRepository.h"
#include "cachelite/database/MySQLConnectionPool.h"
#include "cachelite/database/MySQLRepository.h"

#include "../TestSupport.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using cachelite::database::LookupResult;
using cachelite::database::MySQLConfig;
using cachelite::database::MySQLConnectionPool;
using cachelite::database::MySQLRepository;

void buildsLookupResults() {
    const LookupResult found = LookupResult::found("value");
    CACHELITE_CHECK(found.status == LookupResult::Status::Found);
    CACHELITE_CHECK(found.value == "value");
    CACHELITE_CHECK(found.error.empty());

    const LookupResult missing = LookupResult::notFound();
    CACHELITE_CHECK(missing.status == LookupResult::Status::NotFound);
    CACHELITE_CHECK(missing.value.empty());

    const LookupResult failure = LookupResult::failure("down");
    CACHELITE_CHECK(failure.status == LookupResult::Status::Error);
    CACHELITE_CHECK(failure.error == "down");
}

void keepsPoolLimitConfiguration() {
    MySQLConnectionPool pool(MySQLConfig{}, 4);
    CACHELITE_CHECK(pool.maxConnections() == std::size_t{4});

    MySQLConnectionPool minimumPool(MySQLConfig{}, 0);
    CACHELITE_CHECK(minimumPool.maxConnections() == std::size_t{1});
}

void reportsDisabledBackendClearly() {
#if defined(CACHELITE_ENABLE_MYSQL)
    // The real connection path is covered by an environment-backed integration test.
    CACHELITE_CHECK(true);
#else
    MySQLConnectionPool pool(MySQLConfig{}, 1);
    bool threw = false;
    try {
        static_cast<void>(pool.acquire());
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()).find("CACHELITE_ENABLE_MYSQL=ON") !=
            std::string::npos;
    }
    CACHELITE_CHECK(threw);

    MySQLRepository repository(MySQLConfig{});
    const LookupResult result = repository.find("key");
    CACHELITE_CHECK(result.status == LookupResult::Status::NotFound);
#endif
}

}  // namespace

int main() {
    return cachelite::test::runSuite(
        "database abstractions",
        std::vector<std::pair<std::string, void (*)()>>{
            {"builds lookup results", buildsLookupResults},
            {"keeps pool limit configuration", keepsPoolLimitConfiguration},
            {"reports disabled backend clearly", reportsDisabledBackendClearly},
        }
    );
}

