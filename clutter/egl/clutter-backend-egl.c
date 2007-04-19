#include "config.h"

#include "clutter-backend-egl.h"
#include "clutter-stage-egl.h"
#include "../clutter-private.h"
#include "../clutter-main.h"
#include "../clutter-debug.h"

static ClutterBackendEgl *backend_singleton = NULL;

/* options */
static gchar *clutter_display_name = NULL;
static gint clutter_screen = 0;

/* X error trap */
static int TrappedErrorCode = 0;
static int (*old_error_handler) (Display *, XErrorEvent *);

G_DEFINE_TYPE (ClutterBackendEgl, clutter_backend_egl, CLUTTER_TYPE_BACKEND);

static gboolean
clutter_backend_egl_pre_parse (ClutterBackend  *backend,
                               GError         **error)
{
  const gchar *env_string;

  /* we don't fail here if DISPLAY is not set, as the user
   * might pass the --display command line switch
   */
  env_string = g_getenv ("DISPLAY");
  if (env_string)
    {
      clutter_display_name = g_strdup (env_string);
      env_string = NULL;
    }

  return TRUE;
}

static gboolean
clutter_backend_egl_post_parse (ClutterBackend  *backend,
                                GError         **error)
{
  ClutterBackendEgl *backend_egl = CLUTTER_BACKEND_EGL (backend);

  if (clutter_display_name)
    {
      backend_egl->xdpy = XOpenDisplay (clutter_display_name);
    }
  else
    {
      g_set_error (error, CLUTTER_INIT_ERROR,
                   CLUTTER_INIT_ERROR_BACKEND,
                   "Unable to open display. You have to set the DISPLAY "
                   "environment variable, or use the --display command "
                   "line argument");
      return FALSE;
    }

  if (backend_egl->xdpy)
    {
      CLUTTER_NOTE (MISC, "Getting the X screen");

      if (clutter_screen == 0)
        backend_egl->xscreen = DefaultScreenOfDisplay (backend_egl->xdpy);
      else
        backend_egl->xscreen = ScreenOfDisplay (backend_egl->xdpy,
                                                clutter_screen);

      backend_egl->xscreen_num = XScreenNumberOfScreen (backend_egl->xscreen);
      backend_egl->xwin_root = RootWindow (backend_egl->xdpy,
                                           backend_egl->xscreen_num);
      
      backend_egl->display_name = g_strdup (clutter_display_name);

      /* generic backend properties */
      backend->res_width = WidthOfScreen (backend_egl->xscreen);
      backend->res_height = HeightOfScreen (backend_egl->xscreen);
      backend->mm_width = WidthMMOfScreen (backend_egl->xscreen);
      backend->mm_height = HeightMMOfScreen (backend_egl->xscreen);
      backend->screen_num = backend_egl->xscreen_num;
      backend->n_screens = ScreenCount (backend_egl->xdpy)
    }

  g_free (clutter_display_name);
  
  CLUTTER_NOTE (MISC, "X Display `%s' [%p] opened (screen:%d, root:%u)",
                backend_egl->display_name,
                backend_egl->xdpy,
                backend_egl->xscreen_num,
                (unsigned int) backend_egl->xwin_root);

  return TRUE;
}

static gboolean
clutter_backend_egl_init_stage (ClutterBackend  *backend,
                                GError         **error)
{
  ClutterBackendEgl *backend_egl = CLUTTER_BACKEND_EGL (backend);

  if (!backend_egl->stage)
    {
      ClutterStageEgl *stage_egl;
      ClutterActor *stage;

      stage = g_object_new (CLUTTER_TYPE_STAGE_EGL, NULL);

      /* copy backend data into the stage */
      stage_egl = CLUTTER_STAGE_EGL (stage);
      stage_egl->xdpy = backend_egl->xdpy;
      stage_egl->xwin_root = backend_egl->xwin_root;
      stage_egl->xscreen = backend_egl->xscreen_num;

      g_object_set_data (G_OBJECT (stage), "clutter-backend", backend);

      backend_egl->stage = g_object_ref_sink (stage);
    }

  clutter_actor_realize (backend_egl->stage);
  if (!CLUTTER_ACTOR_IS_REALIZED (backend_egl->stage))
    {
      g_set_error (error, CLUTTER_INIT_ERROR,
                   CLUTTER_INIT_ERROR_INTERNAL,
                   "Unable to realize the main stage");
      return FALSE;
    }

  return TRUE;
}

static void
clutter_backend_egl_init_events (ClutterBackend *backend)
{
  _clutter_events_init (backend);

}

static const GOptionEntry entries[] =
{
  {
    "display", 0,
    G_OPTION_FLAG_IN_MAIN,
    G_OPTION_ARG_STRING, &clutter_display_name,
    "X display to use", "DISPLAY"
  },
  {
    "screen", 0,
    G_OPTION_FLAG_IN_MAIN,
    G_OPTION_ARG_INT, &clutter_screen,
    "X screen to use", "SCREEN"
  },
  { NULL }
};


static void
clutter_backend_egl_add_options (ClutterBackend *backend,
                                 GOptionGroup   *group)
{
  g_option_group_add_entries (group, entries);
}

static ClutterActor *
clutter_backend_egl_get_stage (ClutterBackend *backend)
{
  return NULL;
}

static void
clutter_backend_egl_finalize (GObject *gobject)
{
  ClutterBackendEgl *backend_egl = CLUTTER_BACKEND_EGL (gobject);

  g_free (backend_egl->display_name);

  XCloseDisplay (backend_egl->xdpy);

  if (backend_singleton)
    backend_singleton = NULL;

  G_OBJECT_CLASS (clutter_backend_egl_parent_class)->finalize (gobject);
}

static void
clutter_backend_egl_dispose (GObject *gobject)
{
  ClutterBackendEgl *backend_egl = CLUTTER_BACKEND_EGL (gobject);

  _clutter_events_uninit (CLUTTER_BACKEND (backend_egl));

  if (backend_egl->stage)
    {
      g_object_unref (backend_egl->stage);
      backend_egl->stage = NULL;
    }

  G_OBJECT_CLASS (clutter_backend_egl_parent_class)->dispose (gobject);
}

static GObject *
clutter_backend_egl_constructor (GType                  gtype,
                                 guint                  n_params,
                                 GObjectConstructParam *params)
{
  GObjectClass *parent_class;
  GObject *retval;

  if (!backend_singleton)
    {
      parent_class = G_OBJECT_CLASS (clutter_backend_egl_parent_class);
      retval = parent_class->constructor (gtype, n_params, params);

      backend_singleton = CLUTTER_BACKEND_EGL (retval);

      return retval;
    }

  g_warning ("Attempting to create a new backend object. This should "
             "never happen, so we return the singleton instance.");
  
  return g_object_ref (backend_singleton);
}


static void
clutter_backend_egl_class_init (ClutterBackendEglClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterBackendClass *backend_class = CLUTTER_BACKEND_CLASS (klass);

  gobject_class->constructor = clutter_backend_egl_constructor;
  gobject_class->dispose = clutter_backend_egl_dispose;
  gobject_class->finalize = clutter_backend_egl_finalize;

  backend_class->pre_parse = clutter_backend_egl_pre_parse;
  backend_class->post_parse = clutter_backend_egl_post_parse;
  backend_class->init_stage = clutter_backend_egl_init_stage;
  backend_class->init_events = clutter_backend_egl_init_events;
  backend_class->get_stage = clutter_backend_egl_get_stage;
  backend_class->add_options = clutter_backend_egl_add_options;
}

static void
clutter_backend_egl_init (ClutterBackendEgl *backend_egl)
{
  ClutterBackend *backend = CLUTTER_BACKEND (backend_egl);
  backend->events_queue = g_queue_new ();

  backend->button_click_time[0] = backend->button_click_time[1] = 0;
  backend->button_number[0] = backend->button_number[1] = -1;
  backend->button_x[0] = backend->button_x[1] = 0;
  backend->button_y[0] = backend->button_y[1] = 0;

  backend->res_width = backend->res_height = -1;
  backend->mm_width = backend->mm_height = -1;
  backend->screen_num = 0;
  backend->n_screens = 0;

  backend->double_click_time = 250;
  backend->double_click_distance = 5
}

GType
_clutter_backend_impl_get_type (void)
{
  return clutter_backend_egl_get_type ();
}

static int
error_handler(Display     *xdpy,
	      XErrorEvent *error)
{
  TrappedErrorCode = error->error_code;
  return 0;
}

/**
 * clutter_egl_trap_x_errors:
 *
 * FIXME
 *
 * Since: 0.4
 */
void
clutter_egl_trap_x_errors (void)
{
  TrappedErrorCode  = 0;
  old_error_handler = XSetErrorHandler (error_handler);
}

/**
 * clutter_egl_untrap_x_errors:
 *
 * FIXME
 *
 * Return value: FIXME
 *
 * Since: 0.4
 */
gint
clutter_egl_untrap_x_errors (void)
{
  XSetErrorHandler (old_error_handler);

  return TrappedErrorCode;
}

/**
 * clutter_egl_get_default_display:
 * 
 * FIXME
 *
 * Return value: FIXME
 *
 * Since: 0.4
 */
Display *
clutter_egl_get_default_display (void)
{
  if (!backend_singleton)
    {
      g_critical ("EGL backend has not been initialised");
      return NULL;
    }

  return backend_singleton->xdpy;
}

/**
 * clutter_egl_get_default_screen:
 * 
 * FIXME
 *
 * Return value: FIXME
 *
 * Since: 0.4
 */
gint
clutter_egl_get_default_screen (void)
{
  if (!backend_singleton)
    {
      g_critical ("EGL backend has not been initialised");
      return -1;
    }

  return backend_singleton->xscreen_num;
}

/**
 * clutter_egl_get_default_root_window:
 * 
 * FIXME
 *
 * Return value: FIXME
 *
 * Since: 0.4
 */
Window
clutter_egl_get_default_root_window (void)
{
  if (!backend_singleton)
    {
      g_critical ("EGL backend has not been initialised");
      return None;
    }

  return backend_singleton->xwin_root;
}

EGLDisplay
clutter_egl_display (void)
{
  return (EGLDisplay)clutter_egl_get_default_display ();
}
