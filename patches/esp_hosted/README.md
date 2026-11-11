# esp_hosted local patches

These patches change `managed_components/espressif__esp_hosted`. The IDF
component manager rewrites that directory when it re-resolves dependencies, so
the patches must be applied again after that.

Run:

```
./patches/esp_hosted/apply.sh
```

The script is idempotent. It skips a patch that is already applied.

## 0002-log-the-reason-when-no-host-wakeup-is-needed.patch

The co-processor logs `Wakeup not needed` with no reason when it keeps a packet
from a sleeping host. The patch adds the rule that decided it, so a missed wake
can be traced. It changes logs only.

Upstream this to esp_hosted and delete it here once it is merged.
