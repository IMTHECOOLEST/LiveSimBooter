# Provisioning profile cleanup failure

## Confirmed cause

On 2026-10-24, installing the host app with a newly issued Increased Memory Limit profile failed twice with outer error
`0xe800801a`. The device log showed the actual failure in `misagent`: SQLite error 19, `FOREIGN KEY constraint failed`,
while executing `INSERT INTO profiles VALUES (...)`. The new profile's XML and embedded DER signatures verified locally.
The failure came from the device's existing custom provisioning-database triggers, not a demonstrated entitlement or
signature-format rejection.

The supplied source is [GistMissedRevoke/misdb.sql](/Users/duy/Developer/GistMissedRevoke/misdb.sql). Its uncommitted cleanup
changes added this work to the `BEFORE INSERT ON profiles` trigger:

```sql
UPDATE profiles SET expires = 0 WHERE name = NEW.name;
DELETE FROM profiles WHERE name = NEW.name AND expires = 0;
DELETE FROM certificate_provisioning_cache WHERE uuid = NEW.uuid;
```

They also changed the profile-delete blocker to run only `WHEN OLD.expires != 0`, allowing the cleanup to delete old
same-name profiles. However, `online_auth.uuid` references `profiles.uuid` through `fk_online_auth_profile_uuid` with
`ON DELETE CASCADE`. Its existing `BEFORE DELETE` trigger unconditionally raised
`FAIL, "Trolled online_auth delete"`, preventing deletion of the old profile's child rows.

The resulting foreign-key failure aborts the insert, including the cleanup. The local reproduction preserved the old profile
and its child rows and returned an empty `PRAGMA foreign_key_check`; this failure alone did not establish persistent
database corruption. SQLite's [conflict rules](https://www.sqlite.org/lang_conflict.html) require an abort for a foreign-key
violation even when the operation encounters `FAIL` handling.

The schema has **no foreign key from `profiles.team_id`** to either team table. The earlier missing-team-row hypothesis was
incorrect. The certificate-cache cleanup does not repair the `online_auth` relationship and follows the failing deletion.

## Applied change

With explicit user authorization, only the `online_auth` delete trigger was replaced on the device. The added guard is:

```sql
CREATE TRIGGER trigger_block_delete_online_auth
BEFORE DELETE ON online_auth
WHEN EXISTS (SELECT 1 FROM profiles WHERE uuid = OLD.uuid)
BEGIN
    SELECT RAISE(FAIL, "Trolled online_auth delete");
END;
```

For a direct child deletion while its parent still exists, the original blocker still runs. During the profile's cascading
delete, the parent row has already been removed, so the guard is false and the child deletion can finish. The existing
same-name cleanup trigger and profile-delete blocker were left unchanged. The one-line guard was also mirrored to the
supplied source file with user approval; that separate repository's SQL changes remain uncommitted.

The deployment script checked the expected old trigger definition and replaced it within `BEGIN IMMEDIATE` / `COMMIT`.
It used SQLite's backup operation before changing the trigger. The device backup is:

```text
/var/MobileDevice/ProvisioningProfiles/mis.db.livesim-before-cascade-20261024
```

The backup was set to mode `0600`. The initial shell `umask` attempt failed on this device; the subsequent `chmod` supplied
the intended restriction. No service restart or broader database reset was required.

## Verification

The original failure was reproduced entirely in memory using Python SQLite 3.35.5 and the Mac's SQLite 3.54.0.
The device's SQLite CLI reported 3.34.1. The local comparison isolated the interaction:

| Schema variant | Same-name profile insert with an existing `online_auth` child |
| --- | --- |
| Committed source before cleanup changes | Succeeds; old and new profiles remain |
| Cleanup changes with unconditional child-delete blocker | Fails with `FOREIGN KEY constraint failed`; old rows remain |
| Cleanup changes removed | Succeeds; old and new profiles remain |
| Child-delete blocker removed for the isolated comparison | Succeeds; old profile is replaced |
| Parent-existence guard applied | Succeeds; cascade completes and foreign-key check is empty |

The initial reproduction also tested each other profile child table independently. Certificate, entitlement, XML-profile,
and signing-identity cache rows did not cause the failure. The guarded-trigger test additionally confirmed that direct
`online_auth` deletion still fails and deletion of a protected nonzero-expiry profile is still ignored.

Live readback before the change showed profile `2340b66a-f4ba-4980-ae80-4937d835766b` with 15 `online_auth` rows. After the
change, ordinary `devicectl device install app` succeeded for `com.kdt.LiveSimBooter`. Readback showed the new profile
`cb5e60c5-0140-42a6-a6ab-46553b82f31a` with one authorization row and zero authorization rows for the old profile. Both
profiles used team `UU7W52U79B` and name `iOS Team Provisioning Profile: com.kdt.LiveSimBooter`.

## Scope and retained behavior

Successful cleanup intentionally removes the replaced profile's authorization and other cascading child records. The guard
applies to the database trigger generally, not only LiveSimBooter. It also permits deleting already orphaned `online_auth`
rows because those rows have no parent; it does not authenticate why a deletion was requested.

The existing cleanup matches only `name`, which is neither unique nor team-scoped. It can therefore replace multiple
same-name profiles, including profiles from different teams. The retained `WHEN OLD.expires != 0` condition also does not
block a profile whose expiry is `NULL`. These behaviors were not changed by this targeted repair. This is a record of the
user-authorized repair to an already customized test device, not a requirement for installing LiveSimBooter on other devices.

## Local evidence

The ignored artifacts are retained under `tmp/`:

- `tmp/misdb-review/reproduction.json`: individual child-table reproduction.
- `tmp/misdb-review/comparison.json`: committed/modified schema comparison on both Mac SQLite implementations.
- `tmp/misdb-review/fix-test.json`: guarded-trigger behavior and foreign-key check.
- `tmp/misdb-review/live-before.txt` and `live-after.txt`: device trigger/profile readback.
- `tmp/misdb-review/allow-profile-cascade.sql` and `apply.log`: backup and targeted trigger replacement.
- `tmp/ios27-forced-base/install-retry-security.log`: original live SQLite failure.
- `tmp/ios27-forced-base/install-after-mis-fix.json`: successful ordinary app installation.

Installation success unblocked the separate [iOS 27 cache experiment](shared-cache.md#ios-27-relocation-limits); it does not
establish that the iOS 27 runtime or forced cache mapping works.
