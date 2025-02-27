/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Test for mdn suggestions.

const REMOTE_SETTINGS_DATA = [
  {
    type: "mdn-suggestions",
    attachment: [
      {
        url: "https://example.com/array-filter",
        title: "Array.prototype.filter()",
        description:
          "The filter() method creates a shallow copy of a portion of a given array, filtered down to just the elements from the given array that pass the test implemented by the provided function.",
        keywords: ["array"],
        is_top_pick: true,
      },
    ],
  },
];

add_setup(async function () {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.urlbar.quicksuggest.enabled", true],
      ["browser.urlbar.quicksuggest.nonsponsored", true],
      ["browser.urlbar.quicksuggest.remoteSettings.enabled", true],
      ["browser.urlbar.bestMatch.enabled", true],
      ["browser.urlbar.suggest.mdn", true],
    ],
  });

  await QuickSuggestTestUtils.ensureQuickSuggestInit({
    remoteSettingsResults: REMOTE_SETTINGS_DATA,
  });
});

add_task(async function basic() {
  await SpecialPowers.pushPrefEnv({
    set: [["browser.urlbar.mdn.featureGate", true]],
  });
  await waitForSuggestions();

  const suggestion = REMOTE_SETTINGS_DATA[0].attachment[0];
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: suggestion.keywords[0],
  });
  Assert.equal(UrlbarTestUtils.getResultCount(window), 2);

  const { element, result } = await UrlbarTestUtils.getDetailsOfResultAt(
    window,
    1
  );
  Assert.equal(
    result.providerName,
    UrlbarProviderQuickSuggest.name,
    "The result should be from the expected provider"
  );
  Assert.equal(result.payload.provider, "MDNSuggestions");

  const onLoad = BrowserTestUtils.browserLoaded(
    gBrowser.selectedBrowser,
    false,
    suggestion.url
  );
  EventUtils.synthesizeMouseAtCenter(element.row, {});
  await onLoad;
  Assert.ok(true, "Expected page is loaded");

  await PlacesUtils.history.clear();
  await SpecialPowers.popPrefEnv();
});

add_task(async function rowLabel() {
  await SpecialPowers.pushPrefEnv({
    set: [["browser.urlbar.mdn.featureGate", true]],
  });

  const testCases = [
    {
      bestMatch: true,
      expected: "Recommended resource",
    },
    {
      bestMatch: false,
      expected: "Firefox Suggest",
    },
  ];

  for (const { bestMatch, expected } of testCases) {
    await SpecialPowers.pushPrefEnv({
      set: [["browser.urlbar.bestMatch.enabled", bestMatch]],
    });

    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: REMOTE_SETTINGS_DATA[0].attachment[0].keywords[0],
    });
    Assert.equal(UrlbarTestUtils.getResultCount(window), 2);

    const { element } = await UrlbarTestUtils.getDetailsOfResultAt(window, 1);
    const row = element.row;
    Assert.equal(row.getAttribute("label"), expected);

    await SpecialPowers.popPrefEnv();
  }

  await SpecialPowers.popPrefEnv();
});

add_task(async function disable() {
  await SpecialPowers.pushPrefEnv({
    set: [["browser.urlbar.mdn.featureGate", false]],
  });

  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "array",
  });
  Assert.equal(UrlbarTestUtils.getResultCount(window), 1);

  const { result } = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.equal(result.providerName, "HeuristicFallback");

  await SpecialPowers.popPrefEnv();
});

// Tests the "Not interested" result menu dismissal command.
add_task(async function resultMenu_notInterested() {
  await doDismissTest("not_interested");

  Assert.equal(UrlbarPrefs.get("suggest.mdn"), false);
  const exists = await QuickSuggest.blockedSuggestions.has(
    REMOTE_SETTINGS_DATA[0].attachment[0].url
  );
  Assert.ok(!exists);

  // Re-enable suggestions and wait until MDNSuggestions syncs them from
  // remote settings again.
  UrlbarPrefs.set("suggest.mdn", true);
  await waitForSuggestions();
});

// Tests the "Not relevant" result menu dismissal command.
add_task(async function notRelevant() {
  await doDismissTest("not_relevant");

  Assert.equal(UrlbarPrefs.get("suggest.mdn"), true);
  const exists = await QuickSuggest.blockedSuggestions.has(
    REMOTE_SETTINGS_DATA[0].attachment[0].url
  );
  Assert.ok(exists);

  await QuickSuggest.blockedSuggestions.clear();
});

async function doDismissTest(command) {
  await SpecialPowers.pushPrefEnv({
    set: [["browser.urlbar.mdn.featureGate", true]],
  });
  await waitForSuggestions();

  const keyword = REMOTE_SETTINGS_DATA[0].attachment[0].keywords[0];
  // Do a search.
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: keyword,
  });

  // Check the result.
  const resultCount = 2;
  Assert.equal(
    UrlbarTestUtils.getResultCount(window),
    resultCount,
    "There should be two results"
  );

  const resultIndex = 1;
  let details = await UrlbarTestUtils.getDetailsOfResultAt(window, resultIndex);
  Assert.equal(
    details.result.providerName,
    UrlbarProviderQuickSuggest.name,
    "The result should be from the expected provider"
  );
  Assert.equal(
    details.result.payload.telemetryType,
    "mdn",
    "The result should be a MDN result"
  );

  // Click the command.
  await UrlbarTestUtils.openResultMenuAndClickItem(
    window,
    ["[data-l10n-id=firefox-suggest-command-dont-show-mdn]", command],
    { resultIndex, openByMouse: true }
  );

  // The row should be a tip now.
  Assert.ok(gURLBar.view.isOpen, "The view should remain open after dismissal");
  Assert.equal(
    UrlbarTestUtils.getResultCount(window),
    resultCount,
    "The result count should not haved changed after dismissal"
  );
  details = await UrlbarTestUtils.getDetailsOfResultAt(window, resultIndex);
  Assert.equal(
    details.type,
    UrlbarUtils.RESULT_TYPE.TIP,
    "Row should be a tip after dismissal"
  );
  Assert.equal(
    details.result.payload.type,
    "dismissalAcknowledgment",
    "Tip type should be dismissalAcknowledgment"
  );
  Assert.ok(
    !details.element.row.hasAttribute("feedback-acknowledgment"),
    "Row should not have feedback acknowledgment after dismissal"
  );

  // Get the dismissal acknowledgment's "Got it" button and click it.
  const gotItButton = UrlbarTestUtils.getButtonForResultIndex(
    window,
    0,
    resultIndex
  );
  Assert.ok(gotItButton, "Row should have a 'Got it' button");
  EventUtils.synthesizeMouseAtCenter(gotItButton, {}, window);

  // The view should remain open and the tip row should be gone.
  Assert.ok(
    gURLBar.view.isOpen,
    "The view should remain open clicking the 'Got it' button"
  );
  Assert.equal(
    UrlbarTestUtils.getResultCount(window),
    resultCount - 1,
    "The result count should be one less after clicking 'Got it' button"
  );
  for (let i = 0; i < UrlbarTestUtils.getResultCount(window); i++) {
    details = await UrlbarTestUtils.getDetailsOfResultAt(window, i);
    Assert.ok(
      details.type != UrlbarUtils.RESULT_TYPE.TIP &&
        details.result.payload.telemetryType !== "mdn",
      "Tip result and suggestion should not be present"
    );
  }

  gURLBar.handleRevert();

  // Do the search again.
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: keyword,
  });
  for (let i = 0; i < UrlbarTestUtils.getResultCount(window); i++) {
    details = await UrlbarTestUtils.getDetailsOfResultAt(window, i);
    Assert.ok(
      details.type != UrlbarUtils.RESULT_TYPE.TIP &&
        details.result.payload.telemetryType !== "mdn",
      "Tip result and suggestion should not be present"
    );
  }

  await UrlbarTestUtils.promisePopupClose(window);
  await SpecialPowers.popPrefEnv();
}

async function waitForSuggestions() {
  const keyword = REMOTE_SETTINGS_DATA[0].attachment[0].keywords[0];
  const feature = QuickSuggest.getFeature("MDNSuggestions");
  await TestUtils.waitForCondition(async () => {
    const suggestions = await feature.queryRemoteSettings(keyword);
    return !!suggestions.length;
  }, "Waiting for MDNSuggestions to serve remote settings suggestions");
}
