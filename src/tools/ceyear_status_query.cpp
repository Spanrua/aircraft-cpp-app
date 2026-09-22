#include "services/CeyearVisaQuery.h"

#include <visa.h>

#include <QByteArray>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string visaStatusText(ViStatus status)
{
    return "VISA status " + std::to_string(status);
}

ViStatus writeCommand(ViSession device, const char *command)
{
    std::string bytes(command);
    if (bytes.empty() || bytes.back() != '\n') {
        bytes.push_back('\n');
    }

    ViUInt32 written = 0;
    return viWrite(device,
                   reinterpret_cast<ViBuf>(bytes.data()),
                   static_cast<ViUInt32>(bytes.size()),
                   &written);
}

ViStatus queryCommand(ViSession device, const char *command, std::string *response)
{
    const ViStatus writeStatus = writeCommand(device, command);
    if (writeStatus < VI_SUCCESS) {
        return writeStatus;
    }

    char buffer[1024] = {};
    ViUInt32 readCount = 0;
    ViStatus readStatus = VI_SUCCESS;
    do {
        readCount = 0;
        readStatus = viRead(device,
                            reinterpret_cast<ViBuf>(buffer),
                            static_cast<ViUInt32>(sizeof(buffer) - 1),
                            &readCount);
        if (readCount > 0) {
            response->append(buffer, readCount);
        }
    } while (readStatus == VI_SUCCESS_MAX_CNT);

    if (readStatus < VI_SUCCESS && response->empty()) {
        return readStatus;
    }

    while (!response->empty() && (response->back() == '\n' || response->back() == '\r')) {
        response->pop_back();
    }
    return readStatus;
}

bool checkStatus(ViStatus status, const std::string &step)
{
    if (status >= VI_SUCCESS) {
        return true;
    }

    std::cerr << step << " failed: " << visaStatusText(status) << std::endl;
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    const QByteArray defaultResource = defaultCeyearVisaResource().toLocal8Bit();
    const char *resource = argc > 1 ? argv[1] : defaultResource.constData();
    std::vector<std::string> commands;
    for (int i = 2; i < argc; ++i) {
        commands.emplace_back(argv[i]);
    }

    const bool probeIfIq = std::any_of(commands.begin(), commands.end(), [](const std::string &arg) {
        return arg == "--probe-if-iq";
    });
    if (probeIfIq) {
        commands.clear();
    }
    for (const std::string &command : commands) {
        if (command.find('?') == std::string::npos) {
            std::cerr << "Refusing non-query SCPI in ceyear_status_query: " << command << std::endl;
            return 2;
        }
    }

    ViSession resourceManager = VI_NULL;
    ViSession device = VI_NULL;

    ViStatus status = viOpenDefaultRM(&resourceManager);
    if (!checkStatus(status, "viOpenDefaultRM")) {
        return 1;
    }

    status = viOpen(resourceManager,
                    reinterpret_cast<ViRsrc>(const_cast<char *>(resource)),
                    VI_NULL,
                    VI_NULL,
                    &device);
    if (!checkStatus(status, std::string("viOpen ") + resource)) {
        viClose(resourceManager);
        return 1;
    }

    bool configured = true;
    configured &= checkStatus(viSetAttribute(device, VI_ATTR_TERMCHAR_EN, VI_TRUE),
                              "VI_ATTR_TERMCHAR_EN");
    configured &= checkStatus(viSetAttribute(device, VI_ATTR_TERMCHAR, '\n'),
                              "VI_ATTR_TERMCHAR");
    configured &= checkStatus(viSetAttribute(device, VI_ATTR_TMO_VALUE, 5000),
                              "VI_ATTR_TMO_VALUE");
    if (!configured) {
        viClose(device);
        viClose(resourceManager);
        return 1;
    }

    std::cout << "RESOURCE: " << resource << std::endl;
    if (commands.empty()) {
        const auto querySet = probeIfIq ? defaultCeyearIfIqProbeQueries() : defaultCeyearStatusQueries();
        for (const auto &query : querySet) {
            std::string response;
            status = queryCommand(device, query.command, &response);
            std::cout << query.label.constData() << " [" << query.command.constData() << "]: ";
            if (status < VI_SUCCESS) {
                std::cout << "ERROR " << visaStatusText(status) << std::endl;
                continue;
            }

            std::cout << response << std::endl;
        }
    } else {
        for (const std::string &command : commands) {
            std::string response;
            status = queryCommand(device, command.c_str(), &response);
            std::cout << "QUERY [" << command << "]: ";
            if (status < VI_SUCCESS) {
                std::cout << "ERROR " << visaStatusText(status) << std::endl;
                continue;
            }

            std::cout << response << std::endl;
        }

        std::string response;
        status = queryCommand(device, "SYSTem:ERRor?", &response);
        std::cout << "QUERY [SYSTem:ERRor?]: ";
        if (status < VI_SUCCESS) {
            std::cout << "ERROR " << visaStatusText(status) << std::endl;
        } else {
            std::cout << response << std::endl;
        }
    }

    viClose(device);
    viClose(resourceManager);
    return 0;
}
