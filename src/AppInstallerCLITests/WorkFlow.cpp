// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "TestCommon.h"
#include <AppInstallerLogging.h>
#include <Manifest/Manifest.h>
#include <AppInstallerDownloader.h>
#include <AppInstallerStrings.h>
#include <Workflows/InstallFlow.h>
#include <Workflows/ShowFlow.h>
#include <Workflows/ShellExecuteInstallerHandler.h>
#include <Workflows/WorkflowBase.h>
#include <Public/AppInstallerRepositorySource.h>
#include <Public/AppInstallerRepositorySearch.h>
#include <Commands/InstallCommand.h>
#include <Commands/ShowCommand.h>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Management::Deployment;
using namespace TestCommon;
using namespace AppInstaller::CLI;
using namespace AppInstaller::CLI::Execution;
using namespace AppInstaller::CLI::Workflow;
using namespace AppInstaller::Manifest;
using namespace AppInstaller::Repository;
using namespace AppInstaller::Utility;


#define REQUIRE_TERMINATED_WITH(_context_,_hr_) \
    REQUIRE(_context_.IsTerminated()); \
    REQUIRE(_hr_ == _context_.GetTerminationHR())

struct TestSource : public ISource
{
    struct TestApplication : public IApplication
    {
        TestApplication(const Manifest manifest) : m_manifest(manifest) {}

        std::optional<Manifest> GetManifest(const NormalizedString&, const NormalizedString&) override
        {
            return m_manifest;
        }

        std::string GetId() override
        {
            return m_manifest.Id;
        }

        std::string GetName() override
        {
            return m_manifest.Name;
        }

        std::vector<VersionAndChannel> GetVersions() override
        {
            std::vector<VersionAndChannel> result;
            result.emplace_back(Version(m_manifest.Version), Channel(m_manifest.Channel));
            return result;
        }

        Manifest m_manifest;
    };

    SearchResult Search(const SearchRequest& request) override
    {
        SearchResult result;
        if (request.Query.has_value())
        {
            if (request.Query.value().Value == "TestQueryReturnOne")
            {
                auto manifest = Manifest::CreateFromPath(TestDataFile("InstallFlowTest_Exe.yaml"));
                result.Matches.emplace_back(
                    ResultMatch(
                        std::make_unique<TestApplication>(manifest),
                        ApplicationMatchFilter(ApplicationMatchField::Id, MatchType::Exact, "TestQueryReturnOne")));
            }
            else if (request.Query.value().Value == "TestQueryReturnTwo")
            {
                auto manifest = Manifest::CreateFromPath(TestDataFile("InstallFlowTest_Exe.yaml"));
                result.Matches.emplace_back(
                    ResultMatch(
                        std::make_unique<TestApplication>(manifest),
                        ApplicationMatchFilter(ApplicationMatchField::Id, MatchType::Exact, "TestQueryReturnTwo")));

                auto manifest2 = Manifest::CreateFromPath(TestDataFile("Manifest-Good.yaml"));
                result.Matches.emplace_back(
                    ResultMatch(
                        std::make_unique<TestApplication>(manifest2),
                        ApplicationMatchFilter(ApplicationMatchField::Id, MatchType::Exact, "TestQueryReturnTwo")));
            }
        }
        return result;
    }

    const SourceDetails& GetDetails() const override { THROW_HR(E_NOTIMPL); }
};

struct TestContext;

struct WorkflowTaskOverride
{
    WorkflowTaskOverride(WorkflowTask::Func f, const std::function<void(TestContext&)>& o) :
        Target(f), Override(o) {}

    WorkflowTaskOverride(std::string_view n, const std::function<void(TestContext&)>& o) :
        Target(n), Override(o) {}

    WorkflowTaskOverride(const WorkflowTask& t, const std::function<void(TestContext&)>& o) :
        Target(t), Override(o) {}

    bool Used = false;
    WorkflowTask Target;
    std::function<void(TestContext&)> Override;
};

// Enables overriding the behavior of specific workflow tasks.
struct TestContext : public Context
{
    TestContext(std::ostream& out, std::istream& in) : Context(out, in) {}

    ~TestContext()
    {
        for (const auto& wto : m_overrides)
        {
            if (!wto.Used)
            {
                FAIL("Unused override");
            }
        }
    }

    bool ShouldExecuteWorkflowTask(const Workflow::WorkflowTask& task) override
    {
        auto itr = std::find_if(m_overrides.begin(), m_overrides.end(), [&](const WorkflowTaskOverride& wto) { return wto.Target == task; });

        if (itr == m_overrides.end())
        {
            return true;
        }
        else
        {
            itr->Used = true;
            itr->Override(*this);
            return false;
        }
    }

    void Override(const WorkflowTaskOverride& wto)
    {
        m_overrides.emplace_back(wto);
    }

private:
    std::vector<WorkflowTaskOverride> m_overrides;
};

void OverrideForOpenSource(TestContext& context)
{
    context.Override({ Workflow::OpenSource, [](TestContext& context)
    {
        context.Add<Execution::Data::Source>(std::make_shared<TestSource>());
    } });
}

void OverrideForShellExecute(TestContext& context)
{
    context.Override({ DownloadInstallerFile, [](TestContext& context)
    {
        context.Add<Data::HashPair>({ {}, {} });
        context.Add<Data::InstallerPath>(TestDataFile("AppInstallerTestExeInstaller.exe"));
    } });

    context.Override({ RenameDownloadedInstaller, [](TestContext&)
    {
    } });
}

void OverrideForMSIX(TestContext& context)
{
    context.Override({ MsixInstall, [](TestContext& context)
    {
        std::filesystem::path temp = std::filesystem::temp_directory_path();
        temp /= "TestMsixInstalled.txt";
        std::ofstream file(temp, std::ofstream::out);

        if (context.Contains(Execution::Data::InstallerPath))
        {
            file << context.Get<Execution::Data::InstallerPath>().u8string();
        }
        else
        {
            file << context.Get<Execution::Data::Installer>()->Url;
        }

        file.close();
    } });
}

TEST_CASE("ExeInstallFlowWithTestManifest", "[InstallFlow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Exe.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/custom") != std::string::npos);
    REQUIRE(installResultStr.find("/silentwithprogress") != std::string::npos);
}

TEST_CASE("InstallFlowWithNonApplicableArchitecture", "[InstallFlow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_NoApplicableArchitecture.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_NO_APPLICABLE_INSTALLER);

    // Verify Installer was not called
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
}

TEST_CASE("MsixInstallFlow_DownloadFlow", "[InstallFlow]")
{
    TestCommon::TempFile installResultPath("TestMsixInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    OverrideForMSIX(context);
    // Todo: point to files from our repo when the repo goes public
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Msix_DownloadFlow.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and a local file is used as package Uri.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    Uri uri = Uri(ConvertToUTF16(installResultStr));
    REQUIRE(uri.SchemeName() == L"file");
}

TEST_CASE("MsixInstallFlow_StreamingFlow", "[InstallFlow]")
{
    TestCommon::TempFile installResultPath("TestMsixInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    OverrideForMSIX(context);
    // Todo: point to files from our repo when the repo goes public
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Msix_StreamingFlow.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and a http address is used as package Uri.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    Uri uri = Uri(ConvertToUTF16(installResultStr));
    REQUIRE(uri.SchemeName() == L"https");
}

TEST_CASE("ShellExecuteHandlerInstallerArgs", "[InstallFlow]")
{
    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        // Default Msi type with no args passed in, no switches specified in manifest
        auto manifest = Manifest::CreateFromPath(TestDataFile("InstallerArgTest_Msi_NoSwitches.yaml"));
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context.Add<Data::InstallerPath>(TestDataFile("AppInstallerTestExeInstaller.exe"));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/passive") != std::string::npos);
        REQUIRE(installerArgs.find("AppInstallerTestExeInstaller.exe.log") != std::string::npos);
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        // Msi type with /silent and /log and /custom and /installlocation, no switches specified in manifest
        auto manifest = Manifest::CreateFromPath(TestDataFile("InstallerArgTest_Msi_NoSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log");
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir");
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/quiet") != std::string::npos);
        REQUIRE(installerArgs.find("/log \"MyLog.log\"") != std::string::npos);
        REQUIRE(installerArgs.find("TARGETDIR=\"MyDir\"") != std::string::npos);
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        // Msi type with /silent and /log and /custom and /installlocation, switches specified in manifest
        auto manifest = Manifest::CreateFromPath(TestDataFile("InstallerArgTest_Msi_WithSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log");
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir");
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/mysilent") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mylog=\"MyLog.log\"") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mycustom") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/myinstalldir=\"MyDir\"") != std::string::npos); // Use declaration in manifest
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        // Default Inno type with no args passed in, no switches specified in manifest
        auto manifest = Manifest::CreateFromPath(TestDataFile("InstallerArgTest_Inno_NoSwitches.yaml"));
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context.Add<Data::InstallerPath>(TestDataFile("AppInstallerTestExeInstaller.exe"));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/SILENT") != std::string::npos);
        REQUIRE(installerArgs.find("AppInstallerTestExeInstaller.exe.log") != std::string::npos);
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        // Inno type with /silent and /log and /custom and /installlocation, no switches specified in manifest
        auto manifest = Manifest::CreateFromPath(TestDataFile("InstallerArgTest_Inno_NoSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log");
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir");
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/VERYSILENT") != std::string::npos);
        REQUIRE(installerArgs.find("/LOG=\"MyLog.log\"") != std::string::npos);
        REQUIRE(installerArgs.find("/DIR=\"MyDir\"") != std::string::npos);
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        // Inno type with /silent and /log and /custom and /installlocation, switches specified in manifest
        auto manifest = Manifest::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log");
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir");
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/mysilent") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mylog=\"MyLog.log\"") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mycustom") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/myinstalldir=\"MyDir\"") != std::string::npos); // Use declaration in manifest
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        // Override switch specified. The whole arg passed to installer is overrided.
        auto manifest = Manifest::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log");
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir");
        context.Args.AddArg(Execution::Args::Type::Override, "/OverrideEverything");
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs == "/OverrideEverything"); // Use value specified in override switch
    }
}

TEST_CASE("InstallFlow_SearchAndInstall", "[InstallFlow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    OverrideForOpenSource(context);
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Query, "TestQueryReturnOne");

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/custom") != std::string::npos);
    REQUIRE(installResultStr.find("/silentwithprogress") != std::string::npos);
}

TEST_CASE("InstallFlow_SearchFoundNoApp", "[InstallFlow]")
{
    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    OverrideForOpenSource(context);
    context.Args.AddArg(Execution::Args::Type::Query, "TestQueryReturnZero");

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify proper message is printed
    REQUIRE(installOutput.str().find("No app found matching input criteria.") != std::string::npos);
}

TEST_CASE("InstallFlow_SearchFoundMultipleApp", "[InstallFlow]")
{
    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    OverrideForOpenSource(context);
    context.Args.AddArg(Execution::Args::Type::Query, "TestQueryReturnTwo");

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify proper message is printed
    REQUIRE(installOutput.str().find("Multiple apps found matching input criteria. Please refine the input.") != std::string::npos);
}

TEST_CASE("InstallFlow_SearchAndShowAppInfo", "[ShowFlow]")
{
    std::ostringstream showOutput;
    TestContext context{ showOutput, std::cin };
    OverrideForOpenSource(context);
    context.Args.AddArg(Execution::Args::Type::Query, "TestQueryReturnOne");

    ShowCommand show({});
    show.Execute(context);
    INFO(showOutput.str());

    // Verify AppInfo is printed
    REQUIRE(showOutput.str().find("AppInstallerCliTest.TestInstaller") != std::string::npos);
    REQUIRE(showOutput.str().find("AppInstaller Test Installer") != std::string::npos);
    REQUIRE(showOutput.str().find("1.0.0.0") != std::string::npos);
    REQUIRE(showOutput.str().find("https://ThisIsNotUsed") != std::string::npos);
}

TEST_CASE("InstallFlow_SearchAndShowAppVersion", "[ShowFlow]")
{
    std::ostringstream showOutput;
    TestContext context{ showOutput, std::cin };
    OverrideForOpenSource(context);
    context.Args.AddArg(Execution::Args::Type::Query, "TestQueryReturnOne");
    context.Args.AddArg(Execution::Args::Type::ListVersions);

    ShowCommand show({});
    show.Execute(context);
    INFO(showOutput.str());

    // Verify App version is printed
    REQUIRE(showOutput.str().find("1.0.0.0") != std::string::npos);
    // No manifest info is printed
    REQUIRE(showOutput.str().find("  Download Url: https://ThisIsNotUsed") == std::string::npos);
}