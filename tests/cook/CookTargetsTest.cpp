#include "application/TestCase.h"

#include "core/Logger.h"
#include "io/CookTargets.h"

#include <algorithm>

namespace sparkle
{
// The cook target table and the '+'-separated target grammar --cook_targets accepts. The table is
// generated from cook_targets.json, which build.py and dev/ci_matrix.py read too, so these
// assertions are what keeps the engine's view of a target agreeing with the pipeline's. Runs
// anywhere: no store, no cooker, no RHI.
class CookTargetsTest : public TestCase
{
    Result OnTick(AppFramework & /*app*/) override
    {
        bool success = VerifyTableIsPopulated();
        success &= VerifyPlatformTargetIsKnown();
        success &= VerifyParseAcceptsValidLists();
        success &= VerifyParseRejectsUnknownTargets();

        return success ? Result::Pass : Result::Fail;
    }

    static bool VerifyTableIsPopulated()
    {
        bool success = Expect(!CookTargets::All().empty(), "the generated table is not empty");
        success &= Expect(!CookTargets::Names().empty(), "the diagnostic name list is not empty");

        // every target the table names must resolve, and nothing else may
        for (const auto &[target, family] : CookTargets::All())
        {
            success &= Expect(CookTargets::Contains(target), "a table entry resolves");
            success &= Expect(CookTargets::FamilyOf(target) == family, "FamilyOf agrees with the table");
            success &=
                Expect(CookTargets::Names().find(target) != std::string::npos, "the name list mentions every target");
        }
        success &= Expect(!CookTargets::Contains("nonexistent-target"), "an unknown target does not resolve");
        return success;
    }

    static bool VerifyPlatformTargetIsKnown()
    {
        // PlatformFamily asserts on an unknown target, so a missing self target is a build error
        // rather than a runtime miss
        bool success =
            Expect(CookTargets::Contains(CookTargets::SelfTarget()), "this build's own target is in the table");
        success &= Expect(CookTargets::PlatformFamily() == CookTargets::FamilyOf(CookTargets::SelfTarget()),
                          "the platform family is the self target's family");
        return success;
    }

    static bool VerifyParseAcceptsValidLists()
    {
        // an empty config resolves to the target this binary cooks for
        bool success = Expect(CookTargets::Parse("") == std::vector<std::string>{CookTargets::SelfTarget()},
                              "an empty list defaults to the self target");

        const auto &table = CookTargets::All();
        const std::string first = table.begin()->first;
        const std::string last = table.rbegin()->first;

        success &= Expect(CookTargets::Parse(first) == std::vector<std::string>{first}, "a single target parses");

        if (first != last)
        {
            const auto both = CookTargets::Parse(first + "+" + last);
            success &= Expect(both == std::vector<std::string>({first, last}), "a '+' list parses in order");
            // duplicates collapse, and empty segments are skipped rather than rejected
            const auto deduped = CookTargets::Parse(first + "+" + last + "+" + first);
            success &= Expect(deduped == std::vector<std::string>({first, last}), "a repeated target appears once");
            success &=
                Expect(CookTargets::Parse("+" + first + "++" + last + "+") == std::vector<std::string>({first, last}),
                       "empty segments are skipped");
        }
        return success;
    }

    static bool VerifyParseRejectsUnknownTargets()
    {
        const std::string known = CookTargets::All().begin()->first;
        bool success = Expect(CookTargets::Parse("nonexistent-target").empty(), "an unknown target rejects the list");
        // one bad entry rejects the whole list rather than silently cooking a subset
        success &=
            Expect(CookTargets::Parse(known + "+nonexistent-target").empty(), "a partially valid list is rejected");
        return success;
    }

    static bool Expect(bool condition, const char *description)
    {
        if (condition)
        {
            Log(Info, "CookTargetsTest: OK - {}", description);
        }
        else
        {
            Log(Error, "CookTargetsTest: FAILED - {}", description);
        }
        return condition;
    }
};

static TestCaseRegistrar<CookTargetsTest> cook_targets_test_registrar("cook_targets");
} // namespace sparkle
