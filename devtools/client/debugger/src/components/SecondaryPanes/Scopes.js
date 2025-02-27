/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import React, { PureComponent } from "react";
import { div, button } from "react-dom-factories";
import PropTypes from "prop-types";
import { showMenu } from "../../context-menu/menu";
import { connect } from "../../utils/connect";
import actions from "../../actions";

import {
  getSelectedFrame,
  getCurrentThread,
  getGeneratedFrameScope,
  getOriginalFrameScope,
  getPauseReason,
  isMapScopesEnabled,
  getLastExpandedScopes,
  getIsCurrentThreadPaused,
} from "../../selectors";
import { getScopes } from "../../utils/pause/scopes";
import { getScopeItemPath } from "../../utils/pause/scopes/utils";
import { clientCommands } from "../../client/firefox";

import { objectInspector } from "devtools/client/shared/components/reps/index";

import "./Scopes.css";

const { ObjectInspector } = objectInspector;

class Scopes extends PureComponent {
  constructor(props) {
    const { why, selectedFrame, originalFrameScopes, generatedFrameScopes } =
      props;

    super(props);

    this.state = {
      originalScopes: getScopes(why, selectedFrame, originalFrameScopes),
      generatedScopes: getScopes(why, selectedFrame, generatedFrameScopes),
      showOriginal: true,
    };
  }

  static get propTypes() {
    return {
      addWatchpoint: PropTypes.func.isRequired,
      expandedScopes: PropTypes.array.isRequired,
      generatedFrameScopes: PropTypes.object,
      highlightDomElement: PropTypes.func.isRequired,
      isLoading: PropTypes.bool.isRequired,
      isPaused: PropTypes.bool.isRequired,
      mapScopesEnabled: PropTypes.bool.isRequired,
      openElementInInspector: PropTypes.func.isRequired,
      openLink: PropTypes.func.isRequired,
      originalFrameScopes: PropTypes.object,
      removeWatchpoint: PropTypes.func.isRequired,
      setExpandedScope: PropTypes.func.isRequired,
      toggleMapScopes: PropTypes.func.isRequired,
      unHighlightDomElement: PropTypes.func.isRequired,
      why: PropTypes.object.isRequired,
      selectedFrame: PropTypes.object,
    };
  }

  // FIXME: https://bugzilla.mozilla.org/show_bug.cgi?id=1774507
  UNSAFE_componentWillReceiveProps(nextProps) {
    const {
      selectedFrame,
      originalFrameScopes,
      generatedFrameScopes,
      isPaused,
    } = this.props;
    const isPausedChanged = isPaused !== nextProps.isPaused;
    const selectedFrameChanged = selectedFrame !== nextProps.selectedFrame;
    const originalFrameScopesChanged =
      originalFrameScopes !== nextProps.originalFrameScopes;
    const generatedFrameScopesChanged =
      generatedFrameScopes !== nextProps.generatedFrameScopes;

    if (
      isPausedChanged ||
      selectedFrameChanged ||
      originalFrameScopesChanged ||
      generatedFrameScopesChanged
    ) {
      this.setState({
        originalScopes: getScopes(
          nextProps.why,
          nextProps.selectedFrame,
          nextProps.originalFrameScopes
        ),
        generatedScopes: getScopes(
          nextProps.why,
          nextProps.selectedFrame,
          nextProps.generatedFrameScopes
        ),
      });
    }
  }

  onToggleMapScopes = () => {
    this.props.toggleMapScopes();
  };

  onContextMenu = (event, item) => {
    const { addWatchpoint, removeWatchpoint } = this.props;

    if (!item.parent || !item.contents.configurable) {
      return;
    }

    if (!item.contents || item.contents.watchpoint) {
      const removeWatchpointLabel = L10N.getStr("watchpoints.removeWatchpoint");

      const removeWatchpointItem = {
        id: "node-menu-remove-watchpoint",
        label: removeWatchpointLabel,
        disabled: false,
        click: () => removeWatchpoint(item),
      };

      const menuItems = [removeWatchpointItem];
      showMenu(event, menuItems);
      return;
    }

    const addSetWatchpointLabel = L10N.getStr("watchpoints.setWatchpoint");
    const addGetWatchpointLabel = L10N.getStr("watchpoints.getWatchpoint");
    const addGetOrSetWatchpointLabel = L10N.getStr(
      "watchpoints.getOrSetWatchpoint"
    );
    const watchpointsSubmenuLabel = L10N.getStr("watchpoints.submenu");

    const addSetWatchpointItem = {
      id: "node-menu-add-set-watchpoint",
      label: addSetWatchpointLabel,
      disabled: false,
      click: () => addWatchpoint(item, "set"),
    };

    const addGetWatchpointItem = {
      id: "node-menu-add-get-watchpoint",
      label: addGetWatchpointLabel,
      disabled: false,
      click: () => addWatchpoint(item, "get"),
    };

    const addGetOrSetWatchpointItem = {
      id: "node-menu-add-get-watchpoint",
      label: addGetOrSetWatchpointLabel,
      disabled: false,
      click: () => addWatchpoint(item, "getorset"),
    };

    const watchpointsSubmenuItem = {
      id: "node-menu-watchpoints",
      label: watchpointsSubmenuLabel,
      disabled: false,
      click: () => addWatchpoint(item, "set"),
      submenu: [
        addSetWatchpointItem,
        addGetWatchpointItem,
        addGetOrSetWatchpointItem,
      ],
    };

    const menuItems = [watchpointsSubmenuItem];
    showMenu(event, menuItems);
  };

  renderWatchpointButton = item => {
    const { removeWatchpoint } = this.props;

    if (
      !item ||
      !item.contents ||
      !item.contents.watchpoint ||
      typeof L10N === "undefined"
    ) {
      return null;
    }

    const { watchpoint } = item.contents;
    return button({
      className: `remove-watchpoint-${watchpoint}`,
      title: L10N.getStr("watchpoints.removeWatchpointTooltip"),
      onClick: e => {
        e.stopPropagation();
        removeWatchpoint(item);
      },
    });
  };

  renderScopesList() {
    const {
      isLoading,
      openLink,
      openElementInInspector,
      highlightDomElement,
      unHighlightDomElement,
      mapScopesEnabled,
      selectedFrame,
      setExpandedScope,
      expandedScopes,
    } = this.props;
    const { originalScopes, generatedScopes, showOriginal } = this.state;

    const scopes =
      (showOriginal && mapScopesEnabled && originalScopes) || generatedScopes;

    function initiallyExpanded(item) {
      return expandedScopes.some(path => path == getScopeItemPath(item));
    }

    if (scopes && !!scopes.length && !isLoading) {
      return div(
        {
          className: "pane scopes-list",
        },
        React.createElement(ObjectInspector, {
          roots: scopes,
          autoExpandAll: false,
          autoExpandDepth: 1,
          client: clientCommands,
          createElement: tagName => document.createElement(tagName),
          disableWrap: true,
          dimTopLevelWindow: true,
          frame: selectedFrame,
          mayUseCustomFormatter: true,
          openLink: openLink,
          onDOMNodeClick: grip => openElementInInspector(grip),
          onInspectIconClick: grip => openElementInInspector(grip),
          onDOMNodeMouseOver: grip => highlightDomElement(grip),
          onDOMNodeMouseOut: grip => unHighlightDomElement(grip),
          onContextMenu: this.onContextMenu,
          setExpanded: (path, expand) =>
            setExpandedScope(selectedFrame, path, expand),
          initiallyExpanded: initiallyExpanded,
          renderItemActions: this.renderWatchpointButton,
          shouldRenderTooltip: true,
        })
      );
    }

    let stateText = L10N.getStr("scopes.notPaused");
    if (this.props.isPaused) {
      if (isLoading) {
        stateText = L10N.getStr("loadingText");
      } else {
        stateText = L10N.getStr("scopes.notAvailable");
      }
    }
    return div(
      {
        className: "pane scopes-list",
      },
      div(
        {
          className: "pane-info",
        },
        stateText
      )
    );
  }

  render() {
    return div(
      {
        className: "scopes-content",
      },
      this.renderScopesList()
    );
  }
}

const mapStateToProps = state => {
  // This component doesn't need any prop when we are not paused
  const selectedFrame = getSelectedFrame(state, getCurrentThread(state));
  if (!selectedFrame) {
    return {};
  }

  const { scope: originalFrameScopes, pending: originalPending } =
    getOriginalFrameScope(state, selectedFrame) || {
      scope: null,
      pending: false,
    };

  const { scope: generatedFrameScopes, pending: generatedPending } =
    getGeneratedFrameScope(state, selectedFrame) || {
      scope: null,
      pending: false,
    };

  return {
    selectedFrame,
    mapScopesEnabled: isMapScopesEnabled(state),
    isLoading: generatedPending || originalPending,
    why: getPauseReason(state, selectedFrame.thread),
    originalFrameScopes,
    generatedFrameScopes,
    expandedScopes: getLastExpandedScopes(state, selectedFrame.thread),
    isPaused: getIsCurrentThreadPaused(state),
  };
};

export default connect(mapStateToProps, {
  openLink: actions.openLink,
  openElementInInspector: actions.openElementInInspectorCommand,
  highlightDomElement: actions.highlightDomElement,
  unHighlightDomElement: actions.unHighlightDomElement,
  toggleMapScopes: actions.toggleMapScopes,
  setExpandedScope: actions.setExpandedScope,
  addWatchpoint: actions.addWatchpoint,
  removeWatchpoint: actions.removeWatchpoint,
})(Scopes);
