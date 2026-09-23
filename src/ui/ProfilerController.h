// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/ProgramLineMap.h"
#include "build/SymbolTable.h"
#include "emu/AttributedProfile.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <utility>

namespace pist {

class CodeEditor;
class DebugSessionController;
class IDebugBackend;

/// Profiling as a mode, in one owner (MAJ-41).
///
/// Hatari starts collecting on continue and zeroes the counters if any
/// breakpoint command is issued mid-run, so the whole flow is "arm while
/// stopped, continue, save at the next stop" — which makes the collecting and
/// guided flags part of the session's state rather than of one action. They
/// live here; the session controller clears them when a session ends, because
/// that is where the rest of the session's lifetime is owned. The window's own
/// state — the transport, the session directory, the editor, the actions — is
/// reached through the `Host` operations below (MIN-89 — the seam used to be a
/// `friend` of MainWindow).
class ProfilerController : public QObject
{
public:
    /// The three actions profiling owns, by role: the controller decides what
    /// each should say, the window knows which QAction each one is.
    enum class ProfileAction { Start, Stop, ToCursor };

    /// What the controller does to and reads from the window it serves.
    struct Host
    {
        /// The debug transport in force (read per use: a launch can replace it).
        std::function<IDebugBackend *()> backend;
        /// The session controller, whose profile-save flag the save path sets.
        std::function<DebugSessionController *()> session;
        /// The current text editor: the cursor line for the guided profile.
        std::function<CodeEditor *()> editor;
        /// The program map, for turning the cursor line into an address.
        std::function<const ProgramLineMap &()> programMap;
        /// The build's symbols, one input of the attribution.
        std::function<const QVector<SymbolEntry> &()> symbols;
        /// The current session's directory, where a save is written.
        std::function<QString()> sessionDir;
        /// One line on the console.
        std::function<void(const QString &)> log;
        /// One line on the profiler dock itself, next to its hot lines.
        std::function<void(const QString &)> showMessage;
        /// Show a finished profile: the dock's table, the dock brought forward
        /// and the current editor's gutter heat, all from one attributed value
        /// (MAJ-44).
        std::function<void(const AttributedProfile &)> showResults;
        /// Emit the results-ready event the remote `profile stop` waits on.
        std::function<void(bool)> resultsReady;
        /// What a profile action should show, by role.
        std::function<void(ProfileAction, bool enabled, const QString &tooltip)> setProfileAction;
    };

    explicit ProfilerController(Host host, QObject *parent = nullptr);

    /// Start collecting (Profile Start): only meaningful while stopped.
    void start();
    /// Save and show what was collected (Profile Stop). False when there was
    /// nothing to save — no stopped session, or no session directory to save
    /// into. The file's arrival shows the results.
    bool stop();
    /// The guided profile: arm a one-shot at the cursor line, start collecting
    /// and resume — the results show themselves on the stop.
    void toCursor();
    /// Parse the saved profile, attribute it to the source (attributedProfile),
    /// hand it to the profiler view, and apply the gutter heat and the console
    /// line from the same value. Emits profileResultsReady either way, which is
    /// what the remote `profile stop` waits on.
    void showResults();

    /// The attributed profile of the last run: what the dock renders, what the
    /// gutter heat is scaled from, and what the remote `profile results` verb
    /// reports. Held here because it is the session's result and not a widget's
    /// contents (MAJ-44) — the two programmatic readers reach it without a dock
    /// existing at all. Empty until showResults() parses a save, and cleared
    /// with the session.
    const AttributedProfile &results() const { return m_results; }

    /// Enable the profile actions from the session/profiling state: Start and
    /// Profile to cursor need stopped-and-not-profiling, Stop and Show need
    /// stopped-and-profiling.
    void syncActions();

    /// Hatari is collecting profile counts (set by start()/toCursor(), cleared
    /// by stop() and session end).
    bool active() const { return m_active; }
    void setActive(bool on) { m_active = on; }

    /// A "profile to cursor line" run is collecting; the next stop saves and
    /// shows, and rebuilds the breakpoint set so the one-shot it armed cannot
    /// fire later if some other stop won the race.
    bool guided() const { return m_guided; }
    void setGuided(bool on) { m_guided = on; }

    /// One owner for the session-end reset of the profiling flags.
    void resetSession();

private:
    Host m_host;
    bool m_active = false;
    bool m_guided = false;
    /// The last run's attributed profile (see results()).
    AttributedProfile m_results;
};

} // namespace pist
