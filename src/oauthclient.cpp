#include "oauthclient.h"

// drivesync.pro defines HAVE_LOCAL_OAUTH only when the git-ignored file
// exists, so a fresh clone builds and simply has no client.
#ifdef HAVE_LOCAL_OAUTH
#include "oauthclient.local.h"
#endif

std::optional<OAuthClient> shippedOAuthClient() {
#ifdef HAVE_LOCAL_OAUTH
    const OAuthClient client{QStringLiteral(DRIVESYNC_OAUTH_ID), QStringLiteral(DRIVESYNC_OAUTH_SECRET)};
    // A copy of the example left empty, or with one value only, is no client:
    // Google would refuse half of one in the browser, after the user clicked.
    if (!client.id.isEmpty() && !client.secret.isEmpty() && client.id.endsWith(".apps.googleusercontent.com"))
        return client;
#endif
    return std::nullopt;
}
