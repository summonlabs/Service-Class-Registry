#include "harness.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "scr/text.hpp"

using namespace scr;

namespace {

std::string random_class_id(scrtest::Random& random) {
  static const char* namespaces[] = {"property.alpha", "property.beta", "property.gamma"};
  static const char* names[] = {"hall", "row", "rack", "cell"};
  const std::string name_space = namespaces[random.below(3)];
  const std::string name = std::string(names[random.below(4)]) + "-" + std::to_string(random.below(4));
  return name_space + "/" + name;
}

}  // namespace

SCR_TEST(property_registry_state_machine) {
  const std::uint64_t seed = scrtest::current_seed();
  std::cout << "  property seed: " << seed << "\n";

  for (int run = 0; run < 3; ++run) {
    scrtest::Random random(seed + static_cast<std::uint64_t>(run) * 7919 + 13);
    Registry registry;
    registry.set_authority(AuthorityId::parse("scr-property").value());
    registry.set_epoch(Epoch::from(1));

    std::vector<RevisionRef> published;
    std::vector<std::string> retired;
    const std::string class_id = "property.core/hall";

    for (int step = 0; step < 80; ++step) {
      // Invariants that must hold after every action.
      SCR_REQUIRE(context, registry.validate_all().ok());
      for (const RevisionRef& reference : published) {
        ClassBinding binding;
        binding.class_id = ClassId::parse(class_id).value();
        binding.revision = reference;
        const auto resolution = registry.resolve(binding);
        SCR_REQUIRE(context, resolution.ok());
        if (resolution.value().state == BindingState::UnknownClass ||
            resolution.value().state == BindingState::UnknownRevision ||
            resolution.value().state == BindingState::DigestMismatch) {
          context.fail("a published revision did not resolve: " + reference.to_display_string(), __FILE__, __LINE__);
          return;
        }
      }

      const bool is_retired = std::find(retired.begin(), retired.end(), class_id) != retired.end();
      const auto record = registry.class_record(ClassId::parse(class_id).value());

      // A retired class refuses every new revision of its generation, so the
      // publish branch is only reachable while the class is active.
      const std::uint64_t choice = is_retired ? (1 + random.below(3)) : random.below(6);
      if (choice == 0 || !record.ok()) {
        // Publish the next revision, strengthening availability on the way.
        const std::uint64_t ppm = 998000 + random.below(1900);
        const ClassRevision candidate = scrtest::make_published_revision(registry, class_id, ppm);
        const auto outcome = registry.publish(candidate, PublishOptions{});
        if (!outcome.ok()) {
          context.fail("a sequential publication was refused: " + outcome.status().to_string(), __FILE__, __LINE__);
          return;
        }
        published.push_back(outcome.value().reference);
        // Replaying the same content is always an idempotent success.
        const auto replay = registry.publish(candidate, PublishOptions{});
        SCR_REQUIRE(context, replay.ok());
        SCR_CHECK(context, replay.value().idempotent);
        SCR_CHECK_EQ(context, replay.value().reference.digest.hex(), outcome.value().reference.digest.hex());
        continue;
      }

      if (choice == 1) {
        // A successor whose lineage does not name the authoritative tip must be
        // refused, and the refusal must not change the index.
        const auto current = registry.class_record(ClassId::parse(class_id).value());
        if (!current.ok()) {
          continue;
        }
        const std::string wrong_lineage = std::to_string(current.value().generation.value()) + "." +
                                          std::to_string(current.value().tip.value()) + "@" +
                                          Digest::of("stale successor").hex();
        const ClassRevision stale =
            scrtest::make_simple_revision(class_id, current.value().generation.value(),
                                          current.value().tip.value() + 1, wrong_lineage, 999000);
        const std::size_t before = registry.revision_count();
        const auto outcome = registry.publish(stale, PublishOptions{});
        SCR_REQUIRE(context, !outcome.ok());
        SCR_CHECK_EQ(context, registry.revision_count(), before);
        continue;
      }

      if (choice == 2) {
        // A composition that weakens an inherited requirement is refused.
        const auto current = registry.class_record(ClassId::parse(class_id).value());
        if (current.ok()) {
          ClassRevision composed = scrtest::make_simple_revision("property.core/composed", 1, 1, "genesis", 998000);
          composed.composes.push_back(CompositionRef::create(ClassId::parse(class_id).value(),
                                                             RevisionRef{current.value().generation, current.value().tip,
                                                                         current.value().tip_digest})
                                          .value());
          const auto outcome = registry.publish(composed, PublishOptions{});
          if (outcome.ok()) {
            context.fail("a weakening composition was accepted", __FILE__, __LINE__);
            return;
          }
        }
        continue;
      }

      if (choice == 3) {
        // Compare two published revisions.
        if (published.size() >= 2) {
          const std::size_t first = static_cast<std::size_t>(random.below(published.size()));
          const std::size_t second = static_cast<std::size_t>(random.below(published.size()));
          const auto report = registry.compare(published[first], published[second]);
          SCR_REQUIRE(context, report.ok());
          if (published[first].digest == published[second].digest) {
            SCR_CHECK_EQ(context, report.value().relation, RevisionRelation::Identical);
          } else {
            SCR_CHECK(context, report.value().relation != RevisionRelation::Identical);
          }
        }
        continue;
      }

      if (choice == 4) {
        const auto explanation = registry.explain(ClassId::parse(class_id).value());
        SCR_REQUIRE(context, explanation.ok());
        SCR_CHECK(context, explanation.value().tip_present);
        for (const RuleViolation& violation : explanation.value().rules) {
          SCR_CHECK_EQ(context, violation.severity, Severity::Advisory);
        }
        continue;
      }

      // Retire once; further retirement is an idempotent success.
      const auto reason = BoundedText::create("property retirement", kRetireReasonMaxBytes);
      const auto outcome =
          registry.retire(ClassId::parse(class_id).value(), reason.value(),
                          Timestamp::from_unix_seconds(1700000000 + static_cast<std::int64_t>(step)).value());
      SCR_REQUIRE(context, outcome.ok());
      if (std::find(retired.begin(), retired.end(), class_id) == retired.end()) {
        retired.push_back(class_id);
      }
      const auto again =
          registry.retire(ClassId::parse(class_id).value(), reason.value(),
                          Timestamp::from_unix_seconds(1700000100 + static_cast<std::int64_t>(step)).value());
      SCR_REQUIRE(context, again.ok());
      SCR_CHECK(context, again.value().already_retired);
    }
  }
}

SCR_TEST(property_store_round_trip_is_exact) {
  const std::uint64_t seed = scrtest::current_seed();
  scrtest::Random random(seed + 977);
  scrtest::TempDirectory directory("property-store");
  const std::filesystem::path root = directory.path() / "store";

  auto store = Store::create(root, AuthorityId::parse("scr-property").value(), Epoch::from(1),
                             Timestamp::from_unix_seconds(1700000000).value());
  SCR_REQUIRE(context, store.ok());

  std::vector<std::string> classes;
  for (int index = 0; index < 6; ++index) {
    classes.push_back(random_class_id(random));
    std::sort(classes.begin(), classes.end());
    classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
  }

  std::vector<ClassBinding> bindings;
  for (int step = 0; step < 40; ++step) {
    const std::string& class_id = classes[static_cast<std::size_t>(random.below(classes.size()))];
    const auto record = store.value().registry().class_record(ClassId::parse(class_id).value());
    if (!record.ok()) {
      const auto outcome = store.value().publish(
          scrtest::make_simple_revision(class_id, 1, 1, "genesis", 998000 + random.below(1900)), PublishOptions{});
      SCR_REQUIRE(context, outcome.ok());
      ClassBinding binding;
      binding.class_id = ClassId::parse(class_id).value();
      binding.revision = outcome.value().publish.reference;
      bindings.push_back(binding);
      continue;
    }
    const auto outcome = store.value().publish(
        scrtest::make_published_revision(store.value().registry(), class_id, 998000 + random.below(1900)),
        PublishOptions{});
    if (!outcome.ok()) {
      context.fail("a sequential durable publication was refused: " + outcome.status().to_string(), __FILE__,
                   __LINE__);
      return;
    }
    ClassBinding binding;
    binding.class_id = ClassId::parse(class_id).value();
    binding.revision = outcome.value().publish.reference;
    bindings.push_back(binding);
  }

  const Sequence sequence = store.value().info().sequence;
  const Digest index_digest = store.value().info().index_digest;
  const std::size_t revisions = store.value().info().revision_count;
  store.value() = Store{};

  auto reopened = Store::open(root, StoreMode::ReadOnly);
  SCR_REQUIRE(context, reopened.ok());
  SCR_CHECK_EQ(context, reopened.value().info().sequence.value(), sequence.value());
  SCR_CHECK_EQ(context, reopened.value().info().index_digest.hex(), index_digest.hex());
  SCR_CHECK_EQ(context, reopened.value().info().revision_count, revisions);
  SCR_CHECK(context, reopened.value().registry().validate_all().ok());

  for (const ClassBinding& binding : bindings) {
    const auto resolution = reopened.value().registry().resolve(binding);
    SCR_REQUIRE(context, resolution.ok());
    if (resolution.value().state == BindingState::UnknownClass ||
        resolution.value().state == BindingState::UnknownRevision ||
        resolution.value().state == BindingState::DigestMismatch) {
      context.fail("a binding did not survive the reopen: " + binding.to_string(), __FILE__, __LINE__);
      return;
    }
  }

  const auto report = reopened.value().verify();
  SCR_REQUIRE(context, report.ok());
  SCR_CHECK(context, report.value().ok);
}

SCR_TEST(property_canonical_form_is_order_independent) {
  const std::uint64_t seed = scrtest::current_seed();
  scrtest::Random random(seed + 4241);

  const std::vector<std::string> obligation_lines = {
      "obligation availability.target_ppm required 999000",
      "obligation availability.max_annual_downtime_seconds required 300",
      "obligation redundancy.topology required n+1",
      "obligation redundancy.minimum_independent_fault_domains required 2",
      "obligation redundancy.concurrent_fault_tolerance required 1",
      "obligation power.feed_count required 2",
      "obligation power.path_independence required dual-independent",
      "obligation cooling.mode required redundant-mechanical",
      "obligation cooling.independent_paths required 2",
      "obligation recovery.time_objective_seconds required 120",
      "obligation recovery.restore_mode required warm",
      "obligation operations.monitoring_interval_seconds required 60",
  };

  std::string canonical_digest;
  for (int attempt = 0; attempt < 25; ++attempt) {
    std::vector<std::string> lines = obligation_lines;
    for (std::size_t index = lines.size(); index > 1; --index) {
      const std::size_t swap_with = static_cast<std::size_t>(random.below(index));
      std::swap(lines[index - 1], lines[swap_with]);
    }
    std::string source = "scr-class 1\nclass property.core/order\ngeneration 1\nrevision 1\nlineage genesis\n";
    for (const std::string& line : lines) {
      source += line + "\n";
    }
    source += "text title \"order independence\"\n";
    const auto parsed = parse_scx(source);
    if (!parsed.ok()) {
      context.fail("a generated canonical text source did not parse: " + parsed.status().to_string(), __FILE__,
                   __LINE__);
      return;
    }
    const auto digest = revision_frame_digest(parsed.value());
    SCR_REQUIRE(context, digest.ok());
    if (attempt == 0) {
      canonical_digest = digest.value().hex();
    } else if (digest.value().hex() != canonical_digest) {
      context.fail("insertion order changed the canonical digest on attempt " + std::to_string(attempt), __FILE__,
                   __LINE__);
      return;
    }
  }
}
