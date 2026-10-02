# E-Ink Calendar Google API

This Google Apps Script web app exposes a small, read-only JSON feed containing:

- events from the Google calendars currently selected in Google Calendar;
- tasks from Google Tasks;
- notes from one dedicated Google Tasks list named `记事本`;
- the current date and generation time in `Asia/Shanghai`.

Subscribed holiday calendars are filtered before events are collected. A
second exact-title filter also removes standard all-day holiday entries from
generic calendar subscriptions (for example, 中元节), while ordinary all-day
events in personal and shared calendars are retained.

## Task display rules

An **unfinished** Google Task whose due date is today is moved from `todos` to
`events`, so it appears in the device's **今日日程** panel on that date. The
entry is labelled `待办`, and is placed before ordinary calendar events.

When a task is marked complete in Google Tasks, it is omitted from `todos` on
the device's next sync. **计划日程** is a rolling three-calendar-day window:
today's task goes to `events`, and tasks on the following two calendar dates go
to `planned_todos`. For example, on August 29, tasks due August 30 or August 31
are shown in **计划日程**. Undated, overdue, and later tasks remain in `todos`,
which the device renders as **待办事项**, until they enter the window.

Google Tasks retains a due **date** but discards a due time, so these entries
are intentionally shown as `待办` rather than as an invented appointment time.

## 记事本

Create a Google Tasks list whose name is exactly **记事本**. Every unfinished
task inside that list becomes one line in the device's left-side **记事本**
panel. The task title is the displayed memo text, and the Google Tasks order is
preserved. These entries are deliberately excluded from **今日日程**,
**计划日程** and **待办事项**, even if they have a due date.

The device shows up to ten note lines at once and displays `+N` when additional
entries exist. Complete or delete an item in Google Tasks to remove it from the
memo pad on the next sync.

## Security model

- Calendar and Tasks access is read-only.
- The web app executes as its owner, so the ESP32 never receives a Google OAuth token.
- Requests must contain a random `key` query parameter.
- Treat the deployment URL and device key as private credentials.

## Deployment settings

- Execute as: **Me**
- Who has access: **Anyone**
- Deployment type: **Web app**

Before deployment, open **Project Settings > Script Properties** and add a property
named `DEVICE_KEY` containing a random 64-character hexadecimal value.
Run `testApi` once to grant Calendar and Tasks read-only access and inspect the result.
