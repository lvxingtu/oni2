/*
 * CompletionStoreConnector.re
 *
 * This implements an updater (reducer + side effects) for completion
 */

open EditorCoreTypes;
open Oni_Core;
open Oni_Model;
open Actions;

module Log = (val Log.withNamespace("Oni2.Store.Completions"));
module Completions = Feature_LanguageSupport.Completions;
module Editor = Feature_Editor.Editor;

module Effects = {
  let requestCompletions =
      (~languageFeatures, ~buffer, ~meet: CompletionMeet.t) =>
    Isolinear.Effect.createWithDispatch(~name="requestCompletions", dispatch => {
      Log.info("Requesting for " ++ Uri.toString(Buffer.getUri(buffer)));

      let completionPromise =
        LanguageFeatures.requestCompletions(~buffer, ~meet, languageFeatures);

      Lwt.on_success(completionPromise, completions => {
        dispatch(CompletionAddItems(meet, completions))
      });
    });

  let applyCompletion =
      (~editor, ~meet: CompletionMeet.t, completion: CompletionItem.t) =>
    Isolinear.Effect.createWithDispatch(~name="applyCompletion", dispatch => {
      let cursor = Vim.Cursor.get();
      let delta =
        Index.toZeroBased(cursor.column)
        - Index.toZeroBased(meet.location.column);

      let _: Vim.Context.t = VimEx.repeatInput(delta, "<BS>");
      let {cursors, _}: Vim.Context.t = VimEx.inputString(completion.label);

      dispatch(
        Editor({
          editorId: Editor.getId(editor),
          msg: CursorsChanged(cursors),
        }),
      );
    });
};

module Actions = {
  let noop = state => (state, Isolinear.Effect.none);

  let update = (f, state: State.t) => (
    {...state, completions: f(state.completions)},
    Isolinear.Effect.none,
  );

  let start = (~buffer, ~meet: CompletionMeet.t, state: State.t) => (
    {
      ...state,
      completions: Completions.initial |> Completions.setMeet(meet),
    },
    Effects.requestCompletions(
      ~languageFeatures=state.languageFeatures,
      ~buffer,
      ~meet,
    ),
  );

  let narrow = (~meet: CompletionMeet.t) =>
    update(Completions.setMeet(meet));

  let stop = state => (
    State.{...state, completions: Completions.initial},
    Isolinear.Effect.none,
  );

  let checkCompletionMeet = (state: State.t) => {
    let editor = Feature_Layout.activeEditor(state.layout);
    let maybeBuffer =
      Buffers.getBuffer(
        Feature_Editor.Editor.getBufferId(editor),
        state.buffers,
      );
    let location = Editor.getPrimaryCursor(editor);

    let suggestEnabled =
      state.configuration
      |> Configuration.getValue(c => c.editorQuickSuggestions);

    let maybeMeet =
      Option.bind(
        maybeBuffer,
        buffer => {
          let {isComment, isString}: SyntaxScope.t =
            Feature_Syntax.getSyntaxScope(
              ~bufferId=Buffer.getId(buffer),
              ~line=location.line,
              ~bytePosition=location.column |> Index.toZeroBased,
              state.syntaxHighlights,
            );

          let shouldCheckCompletion =
            isComment
            && suggestEnabled.comments
            || isString
            && suggestEnabled.strings
            || suggestEnabled.other;

          if (shouldCheckCompletion) {
            CompletionMeet.fromBufferLocation(~location, buffer);
          } else {
            None;
          };
        },
      );

    switch (maybeBuffer, maybeMeet) {
    | (Some(buffer), Some(meet)) =>
      switch (state.completions.meet) {
      | None => start(~buffer, ~meet, state)

      | Some(lastMeet)
          when
            meet.base != lastMeet.base
            && meet == {...lastMeet, base: meet.base} =>
        // Only base has changed, so narrow instead of requesting new completions
        narrow(~meet, state)

      | Some(_) => start(~buffer, ~meet, state)
      }

    | _ => stop(state)
    };
  };

  let applyCompletion = (state: State.t) => {
    let editor = Feature_Layout.activeEditor(state.layout);

    let maybeFocused =
      Option.map(
        i => state.completions.filtered[i].item,
        state.completions.focused,
      );

    switch (maybeFocused, state.completions.meet) {
    | (Some(completion), Some(meet)) => (
        state,
        Effects.applyCompletion(~editor, ~meet, completion),
      )
    | _ => noop(state)
    };
  };
};

let start = () => {
  let updater = (state: State.t) =>
    fun
    | Command("acceptSelectedSuggestion") => Actions.applyCompletion(state)

    | Vim(Feature_Vim.ModeChanged(mode)) when mode == Vim.Types.Insert =>
      Actions.checkCompletionMeet(state)

    | Vim(Feature_Vim.ModeChanged(mode)) when mode != Vim.Types.Insert =>
      Actions.stop(state)

    | ExtensionBufferUpdateQueued(_)
        when Feature_Vim.mode(state.vim) == Vim.Types.Insert =>
      Actions.checkCompletionMeet(state)

    | CompletionAddItems(_meet, items) =>
      Actions.update(Completions.addItems(items), state)

    | Command("selectPrevSuggestion") =>
      Actions.update(Completions.focusPrevious, state)

    | Command("selectNextSuggestion") =>
      Actions.update(Completions.focusNext, state)

    | _ => Actions.noop(state);

  updater;
};
