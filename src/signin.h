#pragma once

#include <QString>
#include <QStringList>
#include <QTextStream>
#include <optional>

#include "backend.h"
#include "oauthclient.h"

// "drive-sync signin": Google sign-in through rclone's own browser flow, so
// the token goes from Google to rclone.conf and never through DriveSync.
// With no DriveSync config it creates the rclone remote first setup uses;
// with one, it refreshes the token of the config's remote with that remote's
// own client (a taken-over setup keeps the client it was made with).

// The remote a new user gets. One of this name that DriveSync did not make
// (another type, another client) is never taken or touched; DriveSync's own
// is reused, signed in again when its token is dead, and made again when it
// has no token at all.
inline constexpr char kNewRemoteName[] = "drive-sync";

// "gdrive:" and "gdrive:sub/dir" give "gdrive".
QString remoteName(const QString &remote);
// The arguments after "rclone" that create the remote and open the browser;
// page, when given, is the file rclone shows when the browser comes back.
QStringList createRemoteArguments(const QString &name, const OAuthClient &client, const QString &page = QString());
// The arguments after "rclone" that sign an existing remote in again.
QStringList refreshTokenArguments(const QString &name, const QString &page = QString());

// The page rclone shows when Google sends the browser back: the template
// compiled in (signin-page.html), its text translated by the installed
// translator and HTML-escaped, braces included, so no translation can add a
// Go template action; the logo inline; language as its lang attribute.
// Empty when the resources are missing.
QByteArray signInPage(const QString &language);

struct SignInTools {
    QString rclone;
    // How long the browser may take. The window stops the command sooner when
    // the user cancels; this bounds it when the window died without doing so.
    int timeoutMs = 10 * 60 * 1000;
    // The client a new remote signs in with; tests pass a fake one so the
    // shipped values never reach a fake rclone's call log.
    std::optional<OAuthClient> client;
    // The language of the page rclone shows after Google, the one the
    // installed translator follows.
    QString language = "en";

    static SignInTools defaults();
};

// "drive-sync signin [--json]" and "drive-sync signin --abandon [--json]".
// --abandon deletes the kNewRemoteName remote only when no DriveSync config
// exists and the section is DriveSync's own (type drive, the shipped client
// id) with no token: what a sign-in cut short leaves. With --json, rclone's
// local sign-in address comes first as {"link": url}, the answer last, and a
// refusal over a remote DriveSync did not make says {"reason":
// "remoteExists"}. SIGTERM and SIGINT stop rclone alone, and the command
// then removes the half-made remote itself.
int runSignInCommand(const QStringList &arguments, const BackendPaths &paths, const SignInTools &tools, QTextStream &out,
                     QTextStream &err);
