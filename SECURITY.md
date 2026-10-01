# Security Policy

## Supported Versions

Security fixes are applied to the latest released version of `dell-rf` and the current `main` branch.

Users are encouraged to update to the latest available release before reporting an issue.

## Reporting a Vulnerability

Please **do not open a public GitHub issue** for a suspected security vulnerability.

Use GitHub's **private vulnerability reporting** feature for this repository where available. Include enough information to reproduce and assess the issue, including:

- affected version or commit;
- Linux distribution and kernel version;
- receiver and device model, including USB IDs where relevant;
- a description of the vulnerability and its potential impact;
- reproducible steps or a minimal proof of concept;
- relevant logs, USB/HID captures, or protocol traces;
- any suggested mitigation or patch, if available.

Please remove credentials, personal information, serial numbers, or other unrelated sensitive data from submitted logs and captures.

## What Is Considered a Security Vulnerability?

Security-relevant issues may include, but are not limited to:

- privilege escalation or unintended execution with elevated privileges;
- unsafe handling of `hidraw` devices or device paths;
- memory-safety issues caused by malformed or unexpected receiver responses;
- command, path, or input-validation vulnerabilities;
- unintended modification of a receiver or device other than the one selected;
- weaknesses that allow pairing, unpairing, or receiver state to be modified without the expected authorization or confirmation;
- vulnerabilities in the installation or udev configuration that grant unintended device access;
- security-sensitive protocol behaviour discovered while using or analysing `dell-rf`;
- vulnerabilities that could allow a malicious USB/HID device to compromise the host through `dell-rf`.

Normal application bugs, unsupported hardware, pairing failures, and feature requests should be reported through the public issue tracker unless they have a security impact.

## Dell Devices and Protocol Vulnerabilities

`dell-rf` is an independent open-source project and is not affiliated with or endorsed by Dell Technologies.

Research performed while developing or using this project may uncover vulnerabilities in Dell receivers, peripherals, firmware, or wireless protocols rather than in `dell-rf` itself.

If you believe a finding affects Dell hardware or software independently of `dell-rf`, please avoid publishing exploitable details until the affected vendor has had a reasonable opportunity to investigate and address the issue.

You may also report such findings privately to this project if they affect how `dell-rf` should behave or if coordination with the project would be useful.

## Disclosure Process

For vulnerabilities affecting `dell-rf`, the maintainer will aim to:

1. acknowledge the report;
2. reproduce and assess the issue;
3. develop and test an appropriate fix;
4. coordinate disclosure with the reporter where appropriate;
5. publish a corrected release and security advisory when warranted.

No fixed remediation deadline is guaranteed, but good-faith security reports will be handled as promptly as practical.

Please allow reasonable time for investigation and remediation before public disclosure.

## Security Expectations

`dell-rf` communicates directly with USB HID hardware and some operations may require elevated privileges. It should therefore be treated as a privileged hardware-management utility.

Users should install releases only from trusted sources, verify what receiver is being operated on, and avoid running modified or untrusted builds with elevated privileges.
