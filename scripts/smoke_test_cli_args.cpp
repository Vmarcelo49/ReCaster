// Smoke test for cli_args.cpp.
#include "cli_args.hpp"
#include <cstdio>
#include <exception>
#include <iostream>

static int failures = 0;

void check(bool ok, const std::string& label) {
    std::cout << (ok ? "  OK   " : "  FAIL ") << label << "\n";
    if (!ok) ++failures;
}

int main() {
    using namespace caster::exe::cli;

    // --help
    {
        char* argv[] = {(char*)"caster", (char*)"--help"};
        auto a = parse(2, argv);
        check(a.help_requested, "--help sets help_requested");
        check(a.mode == Mode::Menu, "--help keeps mode=Menu");
    }

    // no args → Menu
    {
        char* argv[] = {(char*)"caster"};
        auto a = parse(1, argv);
        check(!a.help_requested, "no args → no help");
        check(a.mode == Mode::Menu, "no args → Menu");
        check(a.port == -1, "no args → port=-1 (default)");
        check(a.delay == -1, "no args → delay=-1 (auto)");
        check(a.rollback == -1, "no args → rollback=-1 (config default)");
        check(a.name.empty(), "no args → name empty");
        check(a.peer.empty(), "no args → peer empty");
    }

    // --training
    {
        char* argv[] = {(char*)"caster", (char*)"--training"};
        auto a = parse(2, argv);
        check(a.mode == Mode::Training, "--training → Training");
    }

    // --versus
    {
        char* argv[] = {(char*)"caster", (char*)"--versus"};
        auto a = parse(2, argv);
        check(a.mode == Mode::Versus, "--versus → Versus");
    }

    // --host --port=46318 --delay=4
    {
        char* argv[] = {(char*)"caster", (char*)"--host",
                        (char*)"--port=46318", (char*)"--delay=4"};
        auto a = parse(4, argv);
        check(a.mode == Mode::Host, "--host → Host");
        check(a.port == 46318, "port=46318");
        check(a.delay == 4, "delay=4");
    }

    // --host then --versus → last wins (Versus)
    {
        char* argv[] = {(char*)"caster", (char*)"--host", (char*)"--versus"};
        auto a = parse(3, argv);
        check(a.mode == Mode::Versus, "last mode flag wins (Versus)");
    }

    // --join=192.168.1.10:46318
    {
        char* argv[] = {(char*)"caster", (char*)"--join=192.168.1.10:46318"};
        auto a = parse(2, argv);
        check(a.mode == Mode::Join, "--join=... → Join");
        check(a.peer == "192.168.1.10:46318", "peer=192.168.1.10:46318");
    }

    // --join #ABCD  (space-separated)
    {
        char* argv[] = {(char*)"caster", (char*)"--join", (char*)"#ABCD"};
        auto a = parse(3, argv);
        check(a.mode == Mode::Join, "--join #ABCD → Join");
        check(a.peer == "#ABCD", "peer=#ABCD");
    }

    // --spec=192.168.1.10:46318
    {
        char* argv[] = {(char*)"caster", (char*)"--spec=192.168.1.10:46318"};
        auto a = parse(2, argv);
        check(a.mode == Mode::Spectate, "--spec=... → Spectate");
        check(a.peer == "192.168.1.10:46318", "peer ok");
    }

    // --name="My Player"
    {
        char* argv[] = {(char*)"caster", (char*)"--name=My Player"};
        auto a = parse(2, argv);
        check(a.name == "My Player", "name=My Player");
    }

    // --rollback=7
    {
        char* argv[] = {(char*)"caster", (char*)"--rollback=7"};
        auto a = parse(2, argv);
        check(a.rollback == 7, "rollback=7");
    }

    // Validation: --port=99999 should throw
    {
        char* argv[] = {(char*)"caster", (char*)"--port=99999"};
        bool threw = false;
        try { parse(2, argv); }
        catch (const std::exception&) { threw = true; }
        check(threw, "port=99999 throws");
    }

    // Validation: --delay=99 should throw
    {
        char* argv[] = {(char*)"caster", (char*)"--delay=99"};
        bool threw = false;
        try { parse(2, argv); }
        catch (const std::exception&) { threw = true; }
        check(threw, "delay=99 throws");
    }

    // Validation: --name with 40 chars should throw
    {
        std::string long_name(40, 'x');
        std::string arg = "--name=" + long_name;
        char* argv[] = {(char*)"caster", (char*)arg.c_str()};
        bool threw = false;
        try { parse(2, argv); }
        catch (const std::exception&) { threw = true; }
        check(threw, "name with 40 chars throws (>31)");
    }

    // Unknown arg should throw
    {
        char* argv[] = {(char*)"caster", (char*)"--foobar"};
        bool threw = false;
        try { parse(2, argv); }
        catch (const std::exception&) { threw = true; }
        check(threw, "unknown arg throws");
    }

    // Help text contains key strings
    {
        std::string h = helpText();
        check(h.find("--training") != std::string::npos, "help has --training");
        check(h.find("--join=PEER") != std::string::npos, "help has --join=PEER");
        check(h.find("#ABCD") != std::string::npos, "help has #ABCD example");
        check(h.find("46318") != std::string::npos, "help has 46318");
    }

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    } else {
        std::cout << failures << " TESTS FAILED\n";
        return 1;
    }
}
