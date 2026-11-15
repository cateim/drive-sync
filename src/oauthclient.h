#pragma once

#include <QString>
#include <optional>

// The Google OAuth client of the DriveSync Google Cloud project, which first
// setup signs in with. A desktop client's id and secret are not confidential
// (rclone ships its own the same way), but the repository is public and a
// fork must not sign in as DriveSync, so the values live in a git-ignored
// src/oauthclient.local.h that the maintainer fills by hand.
struct OAuthClient {
    QString id;
    QString secret;
};

// The client this build carries, or nothing when it was built without
// src/oauthclient.local.h (or with that file left empty). Never print it.
std::optional<OAuthClient> shippedOAuthClient();
