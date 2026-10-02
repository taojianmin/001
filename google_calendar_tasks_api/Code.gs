/**
 * Google Calendar + Google Tasks read-only API for the e-ink calendar.
 *
 * Deployment:
 *   - Execute as: Me
 *   - Who has access: Anyone
 *   - URL: https://script.google.com/macros/s/.../exec?key=DEVICE_KEY
 *
 * The web app is intentionally read-only. Store DEVICE_KEY in Script Properties.
 */

const SETTINGS = Object.freeze({
  API_VERSION: 8,
  TIME_ZONE: 'Asia/Shanghai',
  MAX_EVENTS: 8,
  MAX_TODOS: 8,
  MAX_NOTES: 12,
  // An unfinished Google Tasks item in this exact list is rendered in the
  // device's left-side memo pad instead of entering the Todo / schedule flow.
  NOTEPAD_TASK_LIST_NAME: '记事本',
  // The calendar window includes today. Today's tasks are rendered in
  // 今日日程, leaving the next two dates for 计划日程.
  PLANNING_WINDOW_CALENDAR_DAYS: 3,
  CACHE_SECONDS: 60,
});

function doGet(e) {
  const suppliedKey = e && e.parameter ? String(e.parameter.key || '') : '';
  const expectedKey = PropertiesService.getScriptProperties().getProperty('DEVICE_KEY') || '';
  if (!expectedKey || !secureEquals_(suppliedKey, expectedKey)) {
    return jsonOutput_({
      ok: false,
      version: SETTINGS.API_VERSION,
      error: 'unauthorized',
    });
  }

  const now = new Date();
  const dateText = formatDate_(now, 'yyyy-MM-dd');
  const cache = CacheService.getScriptCache();
  // The versioned key prevents an updated deployment from serving stale data.
  const cacheKey = `dashboard-v${SETTINGS.API_VERSION}-${dateText}`;
  const cached = cache.get(cacheKey);

  if (cached) {
    return ContentService.createTextOutput(cached)
      .setMimeType(ContentService.MimeType.JSON);
  }

  try {
    const payload = buildPayload_(now);
    const body = JSON.stringify(payload);
    cache.put(cacheKey, body, SETTINGS.CACHE_SECONDS);
    return ContentService.createTextOutput(body)
      .setMimeType(ContentService.MimeType.JSON);
  } catch (error) {
    console.error(error && error.stack ? error.stack : error);
    return jsonOutput_({
      ok: false,
      version: SETTINGS.API_VERSION,
      date: dateText,
      error: 'server_error',
      message: String(error && error.message ? error.message : error),
    });
  }
}

function buildPayload_(now) {
  const calendarEvents = listTodayEvents_(now);
  const { todos: allTodos, notes } = listTaskContent_();
  const todayText = formatDate_(now, 'yyyy-MM-dd');
  const { dueTodayTasks, plannedTodos, remainingTodos } =
    splitTasksByDueDate_(allTodos, todayText);
  const schedule = buildTodaySchedule_(calendarEvents, dueTodayTasks);
  const pendingTodos = allTodos.filter((item) => !item.done).length;

  return {
    ok: true,
    version: SETTINGS.API_VERSION,
    date: todayText,
    generated_at: formatDate_(now, "yyyy-MM-dd'T'HH:mm:ssZ"),
    timezone: SETTINGS.TIME_ZONE,
    counts: {
      // `events` is the total count of items shown in 今日日程: calendar
      // events plus unfinished Google Tasks due today.
      events: schedule.length,
      calendar_events: calendarEvents.length,
      due_today_todos: dueTodayTasks.length,
      planned_todos: plannedTodos.length,
      todos: remainingTodos.length,
      notes: notes.length,
      pending_todos: pendingTodos,
    },
    events: schedule.slice(0, SETTINGS.MAX_EVENTS),
    planned_todos: plannedTodos.slice(0, SETTINGS.MAX_TODOS).map((todo) => ({
      date: plannedDateLabel_(todo.due_date),
      title: todo.title,
      due: todo.due,
      due_date: todo.due_date,
      list: todo.list,
    })),
    todos: remainingTodos.slice(0, SETTINGS.MAX_TODOS),
    notes: notes.slice(0, SETTINGS.MAX_NOTES),
  };
}

/**
 * Places dated Google Tasks according to the e-ink dashboard's three rules:
 * today -> 今日日程, next two dates in a three-calendar-day window -> 计划日程,
 * undated/overdue/outside the window -> 待办事项.
 * Completed tasks are already excluded by listTaskContent_(), so they do not reach
 * the device.
 */
function splitTasksByDueDate_(todos, todayText) {
  const dueTodayTasks = [];
  const plannedTodos = [];
  const remainingTodos = [];
  const plannedLastDate = addCalendarDays_(
    todayText,
    SETTINGS.PLANNING_WINDOW_CALENDAR_DAYS - 1,
  );

  todos.forEach((todo) => {
    if (!todo.done && todo.due_date === todayText) {
      dueTodayTasks.push(todo);
    } else if (!todo.done && todo.due_date > todayText &&
               todo.due_date <= plannedLastDate) {
      plannedTodos.push(todo);
    } else {
      remainingTodos.push(todo);
    }
  });

  return { dueTodayTasks, plannedTodos, remainingTodos };
}

function addCalendarDays_(dateText, days) {
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(dateText || ''));
  if (!match) return '';
  const date = new Date(Date.UTC(
    Number(match[1]), Number(match[2]) - 1, Number(match[3]) + days, 12,
  ));
  return formatDate_(date, 'yyyy-MM-dd');
}

function plannedDateLabel_(dateText) {
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(dateText || ''));
  if (!match) return '计划';
  return `${Number(match[2])}月${Number(match[3])}日`;
}

/**
 * Builds the feed rendered as 今日日程. Due-today tasks are deliberately put
 * first because the e-ink UI has three visible schedule rows; this guarantees
 * a newly due task is visible instead of being hidden by calendar overflow.
 */
function buildTodaySchedule_(calendarEvents, dueTodayTasks) {
  const taskEvents = dueTodayTasks.map((todo) => ({
    // Google Tasks preserves a due *date* but discards a due time.  Showing
    // “待办” is more truthful than treating its stored midnight timestamp as
    // a real 08:00 appointment in Asia/Shanghai.
    time: '待办',
    title: todo.title,
    all_day: true,
    start: todo.due,
    calendar: todo.list,
    source: 'task',
  }));

  return taskEvents.concat(calendarEvents);
}

/**
 * Excludes subscribed holiday calendars without hiding ordinary all-day
 * events from the user's personal or shared calendars.
 */
function isHolidayCalendar_(calendar) {
  const calendarId = String(calendar && calendar.getId ? calendar.getId() : '')
    .toLowerCase();
  const calendarName = cleanText_(
    calendar && calendar.getName ? calendar.getName() : '',
  );

  // Google-managed regional holiday calendars use IDs such as
  // zh-cn.china#holiday@group.v.calendar.google.com.
  if (calendarId.includes('holiday@group.v.calendar.google.com')) return true;

  // Also covers manually added regional, lunar, or traditional-festival
  // calendars whose IDs are not standard.
  return /节假日|公众假期|法定假日|传统节日|农历|黄历|\bholidays?\b|\bfestivals?\b/i.test(calendarName);
}

/**
 * Some calendar subscriptions expose holiday entries through a calendar whose
 * display name is too generic to identify reliably.  Filter well-known
 * all-day holiday titles as a second line of defence, while keeping normal
 * all-day events in personal calendars visible.
 */
function isHolidayEvent_(calendar, event) {
  if (isHolidayCalendar_(calendar)) return true;
  if (!event.isAllDayEvent()) return false;

  const title = cleanText_(event.getTitle());
  return /^(?:元旦|春节|除夕|元宵节|清明节|劳动节|端午节|中秋节|国庆节|中元节|重阳节|七夕节|腊八节|小年|妇女节|青年节|儿童节|建军节|教师节|圣诞节|平安夜|情人节|万圣节|感恩节|母亲节|父亲节)(?:[（(].*[)）])?$/.test(title);
}

function listTodayEvents_(now) {
  const defaultCalendar = CalendarApp.getDefaultCalendar();
  const selectedCalendars = CalendarApp.getAllCalendars()
    .filter((calendar) => calendar.isSelected());

  if (!selectedCalendars.some((calendar) => calendar.getId() === defaultCalendar.getId())) {
    selectedCalendars.unshift(defaultCalendar);
  }

  const events = [];
  const seen = new Set();

  selectedCalendars
    .forEach((calendar) => {
      calendar.getEventsForDay(now, { max: 50 }).forEach((event) => {
        if (isHolidayEvent_(calendar, event)) return;

        const allDay = event.isAllDayEvent();
        const start = event.getStartTime();
        const identity = `${calendar.getId()}|${event.getId()}|${start.getTime()}`;
        if (seen.has(identity)) return;
        seen.add(identity);

        events.push({
          time: allDay ? '全天' : formatDate_(start, 'HH:mm'),
          title: cleanText_(event.getTitle() || '无标题日程'),
          all_day: allDay,
          start: formatDate_(start, "yyyy-MM-dd'T'HH:mm:ssZ"),
          calendar: cleanText_(calendar.getName()),
          sort_time: allDay ? -1 : start.getTime(),
        });
      });
    });

  events.sort((left, right) => {
    if (left.all_day !== right.all_day) return left.all_day ? -1 : 1;
    if (left.sort_time !== right.sort_time) return left.sort_time - right.sort_time;
    return left.title.localeCompare(right.title, 'zh-CN');
  });

  return events.map((event) => ({
    time: event.time,
    title: event.title,
    all_day: event.all_day,
    start: event.start,
    calendar: event.calendar,
  }));
}

function listTaskContent_() {
  const taskLists = listAllTaskLists_();
  const todos = [];
  const notes = [];

  taskLists.slice(0, 20).forEach((taskList) => {
    const isNotepadList = isNotepadTaskList_(taskList);
    let pageToken;
    let pageCount = 0;

    do {
      const options = {
        maxResults: 100,
        showCompleted: false,
        showHidden: false,
        showDeleted: false,
      };
      if (pageToken) options.pageToken = pageToken;

      const response = Tasks.Tasks.list(taskList.id, options);
      (response.items || []).forEach((task) => {
        // Keep the device's Todo panel focused on actionable work. The API
        // option above avoids completed tasks, while this explicit guard also
        // handles a stale/atypical Tasks response safely.
        if (task.deleted || task.status === 'completed' || !task.title) return;

        const item = {
          title: cleanText_(task.title),
          done: task.status === 'completed',
          due: task.due || '',
          due_date: taskDueDate_(task.due),
          completed: task.completed || '',
          updated: task.updated || '',
          list: cleanText_(taskList.title || ''),
          position: task.position || '',
        };

        if (isNotepadList) {
          notes.push(item);
        } else {
          todos.push(item);
        }
      });

      pageToken = response.nextPageToken;
      pageCount += 1;
    } while (pageToken && pageCount < 5);
  });

  todos.sort((left, right) => {
    if (left.done !== right.done) return left.done ? 1 : -1;

    const leftDue = left.due ? Date.parse(left.due) : Number.MAX_SAFE_INTEGER;
    const rightDue = right.due ? Date.parse(right.due) : Number.MAX_SAFE_INTEGER;
    if (leftDue !== rightDue) return leftDue - rightDue;

    if (left.position !== right.position) {
      return String(left.position).localeCompare(String(right.position));
    }
    return left.title.localeCompare(right.title, 'zh-CN');
  });

  notes.sort((left, right) => {
    if (left.position !== right.position) {
      return String(left.position).localeCompare(String(right.position));
    }
    return left.title.localeCompare(right.title, 'zh-CN');
  });

  return {
    todos: todos.map((todo) => ({
      title: todo.title,
      done: todo.done,
      due: todo.due,
      due_date: todo.due_date,
      completed: todo.completed,
      updated: todo.updated,
      list: todo.list,
    })),
    notes: notes.map((note) => ({
      title: note.title,
      updated: note.updated,
    })),
  };
}

function isNotepadTaskList_(taskList) {
  return cleanText_(taskList && taskList.title) === SETTINGS.NOTEPAD_TASK_LIST_NAME;
}

function taskDueDate_(due) {
  if (!due) return '';

  const parsed = new Date(due);
  if (Number.isNaN(parsed.getTime())) return '';
  return formatDate_(parsed, 'yyyy-MM-dd');
}

function listAllTaskLists_() {
  const taskLists = [];
  let pageToken;

  do {
    const options = { maxResults: 100 };
    if (pageToken) options.pageToken = pageToken;

    const response = Tasks.Tasklists.list(options);
    taskLists.push(...(response.items || []));
    pageToken = response.nextPageToken;
  } while (pageToken);

  return taskLists;
}

function cleanText_(value) {
  return String(value || '')
    .replace(/[\r\n\t]+/g, ' ')
    .replace(/\s{2,}/g, ' ')
    .trim();
}

function formatDate_(date, pattern) {
  return Utilities.formatDate(date, SETTINGS.TIME_ZONE, pattern);
}

function secureEquals_(left, right) {
  left = String(left || '');
  right = String(right || '');
  if (left.length !== right.length) return false;

  let difference = 0;
  for (let index = 0; index < left.length; index += 1) {
    difference |= left.charCodeAt(index) ^ right.charCodeAt(index);
  }
  return difference === 0;
}

function jsonOutput_(value) {
  return ContentService.createTextOutput(JSON.stringify(value))
    .setMimeType(ContentService.MimeType.JSON);
}

/** Run this once in the editor before deployment to verify authorization and data. */
function testApi() {
  const payload = buildPayload_(new Date());
  console.log(JSON.stringify(payload, null, 2));
  return payload;
}
