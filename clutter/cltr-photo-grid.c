#include "cltr-photo-grid.h"
#include "cltr-private.h"

struct CltrPhotoGridCell
{
  Pixbuf      *pixb;
  float        angle;
  CltrTexture *texture;
  gint         anim_step;

  CltrPhotoGridCellState state;
};

struct CltrPhotoGrid
{
  CltrWidget     widget;

  gchar         *img_path;

  int            n_rows;
  int            n_cols;
  int            row_offset; /* where is the first visible row. */

  int            cell_width;
  int            cell_height;

  GList         *cells_tail;
  GList         *cell_active;

  gboolean       is_populated;

  /* animation / zoom etc stuff  */

  /* current anim frame position */
  int                 anim_fps, anim_n_steps, anim_step;

  /* start / end points for animations */
  float               zoom_min, zoom_max, zoom_step;
  float               view_min_x, view_max_x, view_min_y, view_max_y; 
  float               scroll_dist;

  /* Values calucated from above for setting up the GL tranforms and 'view' */
  float               paint_trans_x, paint_trans_y, paint_zoom; 
  int                 paint_start_y;

  GList              *paint_cell_item;

  GMutex             *mutex;

  CltrPhotoGridState  state;
};

static void
cltr_photo_grid_paint(CltrWidget *widget);

static gboolean 
cltr_photo_grid_handle_xevent (CltrWidget *widget, XEvent *xev);

static void
cltr_photo_grid_show(CltrWidget *widget);

static void
cltr_photo_grid_update_visual_state(CltrPhotoGrid *grid);


GMutex*
cltr_photo_grid_mutex(CltrPhotoGrid *grid)
{
  return grid->mutex;
}

void
cltr_photo_grid_set_populated(CltrPhotoGrid *grid, gboolean populated)
{
  grid->is_populated = populated;
}

static void
cltr_photo_grid_handle_xkeyevent(CltrPhotoGrid *grid, XKeyEvent *xkeyev)
{
  KeySym kc;

  kc = XKeycodeToKeysym(xkeyev->display, xkeyev->keycode, 0);

  switch (kc)
    {
    case XK_Left:
    case XK_KP_Left:
      cltr_photo_grid_navigate(grid, CLTR_WEST);
      break;
    case XK_Up:
    case XK_KP_Up:
      cltr_photo_grid_navigate(grid, CLTR_NORTH);
      break;
    case XK_Right:
    case XK_KP_Right:
      cltr_photo_grid_navigate(grid, CLTR_EAST);
      break;
    case XK_Down:	
    case XK_KP_Down:	
      cltr_photo_grid_navigate(grid, CLTR_SOUTH);
      break;
    case XK_Return:
      cltr_photo_grid_activate_cell(grid);
      break;
    default:
      CLTR_DBG("unhandled keysym");
    }
}

static gboolean 
cltr_photo_grid_handle_xevent (CltrWidget *widget, XEvent *xev) 
{
  CltrPhotoGrid* grid = CLTR_PHOTO_GRID(widget);

  switch (xev->type)
    {
    case KeyPress:
      CLTR_DBG("KeyPress");
      cltr_photo_grid_handle_xkeyevent(grid, &xev->xkey);
      break;
    }
  
  return TRUE;
}


CltrPhotoGridCell*
cltr_photo_grid_cell_new(CltrPhotoGrid *grid,
			 Pixbuf        *pixb)
{
  CltrPhotoGridCell *cell = NULL;
  int                   maxw = grid->widget.width, maxh = grid->widget.height;
  int                   neww = 0, newh = 0;
  Pixbuf               *tmp_pixb = NULL;

  cell = g_malloc0(sizeof(CltrPhotoGridCell));

  if (pixb->width > pixb->height) /* landscape */
    {
      if (pixb->width > maxw)
	{
	  neww = maxw;
	  newh = (neww * pixb->height) / pixb->width;
	}
    }
  else                            /* portrait */
    {
      if (pixb->height > maxh)
	{
	  newh = maxh;
	  neww = (newh * pixb->width) / pixb->height;
	}
    }

  if (neww || newh)
    {
      cell->pixb = pixbuf_scale_down(pixb, neww, newh);
      pixbuf_unref(pixb);
    }
  else cell->pixb = pixb;

  cell->texture = cltr_texture_new(cell->pixb);

  cell->angle = 6.0 - (rand()%12);

  cell->anim_step = 15;
  cell->state = CLTR_PHOTO_GRID_CELL_STATE_APPEARING;

  return cell;
}

Pixbuf*
cltr_photo_grid_cell_pixbuf(CltrPhotoGridCell *cell)
{
  return cell->pixb;
}

CltrPhotoGridCell*
cltr_photo_grid_get_active_cell(CltrPhotoGrid     *grid)
{
  if (grid->cell_active)
    return grid->cell_active->data;
  else
    return NULL;
}

void
cltr_photo_grid_set_active_cell(CltrPhotoGrid *grid, CltrPhotoGridCell *cell)
{
  GList *cell_item = NULL;

  cell_item = g_list_find(g_list_first(grid->cells_tail), (gconstpointer)cell);

  if (cell_item)
    grid->cell_active = cell_item;
}

CltrPhotoGridCell*
cltr_photo_grid_get_first_cell(CltrPhotoGrid     *grid)
{
  GList *cell_item = NULL;

  cell_item = g_list_first(grid->cells_tail);

  if (cell_item)
    return cell_item->data;
  return NULL;
}

void
cltr_photo_grid_append_cell(CltrPhotoGrid     *grid,
			    CltrPhotoGridCell *cell)
{
  grid->cells_tail = g_list_append(grid->cells_tail, cell);
} 

/* relative */
static void
ctrl_photo_grid_cell_to_coords(CltrPhotoGrid *grid,
			       GList            *cell,
			       int              *x,
			       int              *y)
{
  int idx;

  idx = g_list_position(grid->cells_tail, cell);  

  *y = idx / grid->n_cols;
  *x = idx % grid->n_cols;

  CLTR_DBG("idx: %i x: %i, y: %i", idx, *x , *y);
}

static void
ctrl_photo_grid_get_zoomed_coords(CltrPhotoGrid *grid,
				  int              x,
				  int              y,
				  float           *tx,
				  float           *ty)
{
  /* 
   * figure out translate co-ords for the cell at x,y to get translated
   * so its centered for glScale to zoom in on it.
  */

  *tx = (float)grid->cell_width  * (grid->zoom_max) * x * -1.0;
  *ty = (float)grid->cell_height * (grid->zoom_max) * y * -1.0;
}

static gboolean
cell_is_offscreen(CltrPhotoGrid *grid,
		  GList            *cell,
		  CltrDirection    *where)
{
  int idx;

  idx = g_list_position(grid->cells_tail, cell);  

  CLTR_DBG("idx %i, rows*cols %i", idx, grid->n_cols * grid->n_rows);

  if (idx < (grid->row_offset * grid->n_cols))
    {
      if (where) *where = CLTR_NORTH;
      return TRUE; 		/* scroll up */
    }

  if (idx >= ((grid->row_offset * grid->n_cols)+(grid->n_cols * grid->n_rows)))
    {
      if (where) *where = CLTR_SOUTH;
      return TRUE; 		/* scroll down */
    }

  return FALSE;
}

gboolean
cltr_photo_grid_idle_cb(gpointer data)
{
  CltrPhotoGrid *grid = (CltrPhotoGrid *)data;

  cltr_photo_grid_update_visual_state(grid);

  cltr_widget_queue_paint(CLTR_WIDGET(grid));

  if (!grid->is_populated)
    return TRUE;

  switch(grid->state)
    {
    case CLTR_PHOTO_GRID_STATE_ZOOM_IN:
    case CLTR_PHOTO_GRID_STATE_ZOOM_OUT:
    case CLTR_PHOTO_GRID_STATE_ZOOMED_MOVE:
    case CLTR_PHOTO_GRID_STATE_SCROLLED_MOVE:
      return TRUE;
    case CLTR_PHOTO_GRID_STATE_ZOOMED:
    case CLTR_PHOTO_GRID_STATE_BROWSE:        
    default:
      return FALSE;  /* no need for rapid updates now  */
    }
}


void
cltr_photo_grid_navigate(CltrPhotoGrid *grid,
			 CltrDirection     direction) 
{
  GList *cell_orig = grid->cell_active;

  switch (direction)
    {
    case CLTR_SOUTH:
      if (g_list_nth(grid->cell_active, grid->n_cols))
	grid->cell_active = g_list_nth(grid->cell_active, grid->n_cols);
      break;
    case CLTR_NORTH:
      if (g_list_nth_prev(grid->cell_active, grid->n_cols))
	grid->cell_active = g_list_nth_prev(grid->cell_active, grid->n_cols);
      break;
    case CLTR_EAST:
      if (g_list_next(grid->cell_active))
	grid->cell_active = g_list_next(grid->cell_active);
      break;
    case CLTR_WEST:
      if (g_list_previous(grid->cell_active))
	grid->cell_active = g_list_previous(grid->cell_active);
      break;
    }

  if (cell_orig != grid->cell_active) /* we've moved */
    {
      int           x, y;
      float         zoom = grid->zoom_min;
      CltrDirection where;

      if (cell_is_offscreen(grid, grid->cell_active, &where))
	{
	  GList *cell_item = NULL;

	  cell_item = g_list_nth(grid->cells_tail, 
				 grid->n_cols * grid->row_offset);

	  if (grid->state != CLTR_PHOTO_GRID_STATE_ZOOMED)
	    grid->state = CLTR_PHOTO_GRID_STATE_SCROLLED_MOVE;
	  
	  /* scroll */
	  if (where == CLTR_NORTH)
	    { 		/* up */
	      grid->scroll_dist = grid->cell_height;
	      grid->row_offset--;
	    }
	  else
	    {
	      grid->scroll_dist = - grid->cell_height;
	      grid->row_offset++;
	    }
	  
	  if (grid->state != CLTR_PHOTO_GRID_STATE_ZOOMED)	      
	    g_timeout_add(FPS_TO_TIMEOUT(grid->anim_fps), 
			  cltr_photo_grid_idle_cb, grid);
	}

      if (grid->state == CLTR_PHOTO_GRID_STATE_ZOOMED)
	{
	  grid->state      = CLTR_PHOTO_GRID_STATE_ZOOMED_MOVE;

	  grid->view_min_x = grid->view_max_x; 
	  grid->view_min_y = grid->view_max_y ;
	  grid->anim_step  = 0;
	  zoom             = grid->zoom_max;

	  g_timeout_add(FPS_TO_TIMEOUT(grid->anim_fps), 
			cltr_photo_grid_idle_cb, grid);
	}
	  
      ctrl_photo_grid_cell_to_coords(grid, grid->cell_active, &x, &y);

      ctrl_photo_grid_get_zoomed_coords(grid, x, y,
					&grid->view_max_x,
					&grid->view_max_y);
				       
      CLTR_DBG("x: %f, y: %f", grid->view_max_x , grid->view_max_y);

      cltr_widget_queue_paint(CLTR_WIDGET(grid));
    }
}

void 				/* bleh badly named */
cltr_photo_grid_activate_cell(CltrPhotoGrid *grid)
{
  if (grid->state == CLTR_PHOTO_GRID_STATE_BROWSE)
    {
      grid->state = CLTR_PHOTO_GRID_STATE_ZOOM_IN;


      g_timeout_add(FPS_TO_TIMEOUT(grid->anim_fps), 
		    cltr_photo_grid_idle_cb, grid);
    }
  else if (grid->state == CLTR_PHOTO_GRID_STATE_ZOOMED)
    {
      grid->state = CLTR_PHOTO_GRID_STATE_ZOOM_OUT;
	/* reset - zoomed moving will have reset */

      grid->view_min_x = 0.0; 
      grid->view_min_y = 0.0; /*- (grid->row_offset * grid->cell_height);*/

      g_timeout_add(FPS_TO_TIMEOUT(grid->anim_fps), 
		    cltr_photo_grid_idle_cb, grid);
    }
}			      


static void
cltr_photo_grid_update_visual_state(CltrPhotoGrid *grid)
{
  int view_x_diff  = grid->view_max_x - grid->view_min_x;
  int view_y_diff  = grid->view_max_y - grid->view_min_y;
  int zoom_diff    = grid->zoom_max - grid->zoom_min;
  int row_offset_h = grid->row_offset * grid->cell_height;

  /* Default states ( zoomed out ) */
  grid->paint_zoom      = grid->zoom_min;
  grid->paint_trans_x   = grid->view_min_x;
  grid->paint_trans_y   = grid->view_min_y - row_offset_h;
  grid->paint_start_y   = row_offset_h;
  grid->paint_cell_item = g_list_nth(grid->cells_tail, 
				     grid->n_cols * grid->row_offset);

  if (grid->state != CLTR_PHOTO_GRID_STATE_BROWSE)
    {
      float scroll_min_y_offset = (float)(row_offset_h);

      /* Assume zoomed in */
      grid->paint_zoom    = grid->zoom_max; 
      grid->paint_trans_x = grid->view_max_x;
      grid->paint_trans_y = grid->view_max_y;

      if (grid->state == CLTR_PHOTO_GRID_STATE_ZOOM_IN)
	{
	  grid->anim_step++;

	  /* Are we zoomed all the way in > */
	  if (grid->anim_step >= grid->anim_n_steps)
	    {
	      grid->state     = CLTR_PHOTO_GRID_STATE_ZOOMED;
	      grid->anim_step = 0;
	    }
	  else 
	    {
	      float f = (float)grid->anim_step/grid->anim_n_steps;

	      scroll_min_y_offset *= grid->zoom_max;

	      grid->paint_zoom = grid->zoom_min + (zoom_diff * f);
	      grid->paint_trans_x = view_x_diff * f;
	      grid->paint_trans_y = (view_y_diff + scroll_min_y_offset) * f;

	      grid->paint_start_y = 0;
	    }
	} 
      else if (grid->state == CLTR_PHOTO_GRID_STATE_ZOOM_OUT)
	{
	  grid->anim_step++;
	  
	  if (grid->anim_step >= grid->anim_n_steps)
	    {
	      grid->paint_zoom     = grid->zoom_min;
	      grid->anim_step      = 0;
	      grid->paint_trans_x  = grid->view_min_x;
	      grid->paint_trans_y  = grid->view_min_y - scroll_min_y_offset; 
	      grid->state          = CLTR_PHOTO_GRID_STATE_BROWSE;
	    }
	  else 
	    {
	      float f = (float)(grid->anim_n_steps - grid->anim_step ) 
		        / grid->anim_n_steps;

	      scroll_min_y_offset *= grid->zoom_max;

	      grid->paint_zoom = grid->zoom_min + (zoom_diff * f);
	      grid->paint_trans_x = view_x_diff * f;
	      grid->paint_trans_y = (view_y_diff + scroll_min_y_offset) * f;
	      grid->paint_start_y = 0;

	    }
	}
      else if (grid->state == CLTR_PHOTO_GRID_STATE_ZOOMED_MOVE)
	{
	  grid->anim_step++;

	  if (grid->anim_step >= grid->anim_n_steps)
	    {
	      grid->state     = CLTR_PHOTO_GRID_STATE_ZOOMED;
	      grid->anim_step = 0;
	    }
	  else
	    {
	      float f = (float)grid->anim_step/grid->anim_n_steps;

	      grid->paint_trans_x = grid->view_min_x + (view_x_diff * f);
	      grid->paint_trans_y = grid->view_min_y + (view_y_diff * f);
	    }
	}
      else if (grid->state == CLTR_PHOTO_GRID_STATE_SCROLLED_MOVE)
	{
	  grid->paint_zoom    = grid->zoom_min;
	  grid->paint_trans_x = grid->view_min_x;
	  grid->paint_trans_y = grid->view_min_y - row_offset_h;
	  grid->anim_step++;

	   if (grid->anim_step >= (grid->anim_n_steps/4))
	    {
	      grid->state      = CLTR_PHOTO_GRID_STATE_BROWSE;
	      grid->anim_step  = 0;
	      grid->paint_zoom = grid->zoom_min;
	    }
	  else
	    {
	      float f = (float)grid->anim_step / (grid->anim_n_steps/4);

	      grid->paint_trans_y += (grid->scroll_dist * f);

	      if (grid->scroll_dist > 0) /* up */
		{
		  grid->paint_start_y = (grid->row_offset-1) * grid->cell_height;
		}
	      else 		/* down */
		{
		  grid->paint_cell_item = g_list_nth(grid->cells_tail, 
						     grid->n_cols * (grid->row_offset-1));
		}
	    }
	}
    }
}

static void
cltr_photo_grid_paint(CltrWidget *widget)
{
  int x = 0, y = 0, rows = 0, cols = 0, i =0;
  GList *cell_item;

  CltrWindow *win = CLTR_WINDOW(widget->parent);
  CltrPhotoGrid *grid = (CltrPhotoGrid *)widget;

  rows = grid->n_rows+1;

  CLTR_MARK();

  glPushMatrix();

  if (grid->cells_tail == NULL)
    {
      /* No pictures to paint yet */
      CltrWindow *win = CLTR_WINDOW(grid->widget.parent);

      glColor3f(0.6, 0.6, 0.62);
      glRecti(0, 0, widget->width, widget->height);

      glPopMatrix();
      return;
    }

  /*
   * Using GL_POLYGON_SMOOTH with 'regular' alpha blends causes ugly seems
   * in the textures and texture tile borders. We therefore do this 'saturate'
   * trick painting front -> back.
   * 
   * see http://blog.metawrap.com/blog/PermaLink.aspx?guid=db82f92e-9fc8-4635-b3e5-e37a1ca6ee0a 
   * for more info
   *
   * Note bg must be glClearColor( 0.0, 0.0, 0.0, 0.0 ) to work.
   * Is there a better way.?
   *  - multisample ?  
  */

  if (!grid->paint_cell_item)
    cltr_photo_grid_update_visual_state(grid);

  glEnable(GL_BLEND);

  glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST); /* needed  */

  glEnable(GL_POLYGON_SMOOTH);

  glDisable(GL_LIGHTING); 
  glDisable(GL_DEPTH_TEST);

  glBlendFunc(GL_SRC_ALPHA_SATURATE,GL_ONE);

  glColor4f(1.0, 1.0, 1.0, 1.0);

  /* values from  cltr_photo_grid_update_visual_state() */

  cell_item = grid->paint_cell_item;
  y         = grid->paint_start_y;

  glTranslatef (grid->paint_trans_x, grid->paint_trans_y, 0.0);
  glScalef (grid->paint_zoom, grid->paint_zoom, 0.0);

  while (rows--)
    {
      cols = grid->n_cols;
      x = 0; 
      while (cols--)
	{
	  CltrPhotoGridCell *cell = (CltrPhotoGridCell *)cell_item->data;
	  Pixbuf            *pixb = NULL;
	  int                x1, x2, y1, y2, thumb_w, thumb_h;
	  int                ns_border, ew_border;

	  pixb = cell->pixb;

	  thumb_w = (pixb->width  / grid->n_cols);
	  thumb_h = (pixb->height / grid->n_rows);

	  if (cell->state == CLTR_PHOTO_GRID_CELL_STATE_APPEARING)
	    {
	      cell->anim_step -= 4;

	      if (cell->anim_step <= 0)
		{
		  cell->state = CLTR_PHOTO_GRID_CELL_STATE_STATIC;
		}
	      else
		{
		  thumb_w = thumb_w + cell->anim_step;
		  thumb_h = thumb_h + cell->anim_step;
		}
	      /* set color here for developing effect 
               * only fully develop when all picts loaded ?
               * blur texture too ?
	      */
	      /* glColor4f(1.0, 1.0, 1.0, 0.5); */

	      cell->anim_step = 0;
	    }

	  ew_border = thumb_w/8;
	  ns_border = thumb_h/8; 

	  thumb_w -= (2 * ew_border);
	  thumb_h -= (2 * ns_border);

	  x1 = x + ((grid->cell_width - thumb_w)/2);
	  y1 = y + ((grid->cell_height - thumb_h)/2);

	  x2 = x1 + thumb_w;
	  y2 = y1 + thumb_h;

	  glPushMatrix();

	  /* Translate origin to rotation point ( photo center ) */
	  glTranslatef( x1 + ((x2-x1)/2), y1 + ((y2-y1)/2), 0.0);

	  if (cell->state != CLTR_PHOTO_GRID_CELL_STATE_APPEARING)
	    /* Rotate around Z axis */
	    glRotatef ( cell->angle, 0.0, 0.0, 1.0);

	  glEnable(GL_TEXTURE_2D);

	  g_mutex_lock(grid->mutex);

	  cltr_texture_render_to_gl_quad(cell->texture,
					 -(thumb_w/2),
					 -(thumb_h/2),
					 (thumb_w/2),
					 (thumb_h/2));

	  g_mutex_unlock(grid->mutex);

	  glDisable(GL_TEXTURE_2D);


	  if (cell_item == grid->cell_active 
	      && grid->state == CLTR_PHOTO_GRID_STATE_BROWSE)
	    glColor4f(1.0, 1.0, 1.0, 1.0);
	  else
	    glColor4f(0.9, 0.95, 0.95, 1.0);

	  /* Draw with origin in center of photo */

	  /*
	  glRecti(-(thumb_w/2)-4, -(thumb_h/2)-4, 
		  (thumb_w/2)+4, (thumb_h/2)+ns_border);
	  */

	  cltr_glu_rounded_rect(-(thumb_w/2)-4, -(thumb_h/2)-4, 
				(thumb_w/2)+4, (thumb_h/2)+ns_border,
				thumb_w/30,
				NULL);

	  /* shadow */

	  glColor4f(0.1, 0.1, 0.1, 0.3);

	  cltr_glu_rounded_rect(-(thumb_w/2)-4 + 1, -(thumb_h/2)-4 + 1, 
				(thumb_w/2)+4 + 1, (thumb_h/2)+ns_border +1,
				thumb_w/30,
				NULL);


	  glColor4f(1.0, 1.0, 1.0, 1.0);

	  glEnable(GL_TEXTURE_2D);

	  glPopMatrix();

	  cell_item = g_list_next(cell_item);

	  if (!cell_item)
	    goto finish;

	  x += grid->cell_width;
	  i++;
	}

      y += grid->cell_height;
    }

 finish:

  glPopMatrix();

  /* finally paint background  */

  glDisable(GL_TEXTURE_2D);
  glColor3f(0.6, 0.6, 0.62);
  glRecti(0, 0, widget->width, widget->height);

  /* reset */

  glDisable(GL_POLYGON_SMOOTH);
  glDisable(GL_BLEND);
  glDisable(GL_TEXTURE_2D);
}

static void
cltr_photo_grid_show(CltrWidget *widget)
{
  CltrPhotoGrid *grid = CLTR_PHOTO_GRID(widget);

  /*
  GThread       *loader_thread; 

  loader_thread = g_thread_create (cltr_photo_grid_populate,
				   (gpointer)grid,
				   TRUE,
				   NULL);
  */

  grid->state = CLTR_PHOTO_GRID_STATE_BROWSE;

  if (!grid->is_populated)
    g_timeout_add(FPS_TO_TIMEOUT(20), 
		  cltr_photo_grid_idle_cb, grid);

  cltr_widget_queue_paint(widget);
}

void
cltr_photo_grid_set_fps(CltrPhotoGrid *grid, int fps)
{
  grid->anim_fps = fps;
}

int
cltr_photo_grid_get_fps(CltrPhotoGrid *grid)
{
  return grid->anim_fps;
}

void
cltr_photo_grid_set_anim_steps(CltrPhotoGrid *grid, int steps)
{
  grid->anim_n_steps = steps;
}

int
cltr_photo_grid_get_anim_steps(CltrPhotoGrid *grid)
{
  return grid->anim_n_steps;
}



CltrWidget*
cltr_photo_grid_new(int            width, 
		    int            height,
		    int            n_cols,
		    int            n_rows,
		    const gchar   *img_path)
{
  CltrPhotoGrid *grid = NULL;

  grid = g_malloc0(sizeof(CltrPhotoGrid));

  grid->widget.width  = width;
  grid->widget.height = height;

  grid->widget.show   = cltr_photo_grid_show;
  grid->widget.paint  = cltr_photo_grid_paint;

  grid->widget.xevent_handler = cltr_photo_grid_handle_xevent;

  grid->img_path = strdup(img_path);
  grid->n_cols = n_cols;
  grid->n_rows = n_rows;

  grid->cell_width  = grid->widget.width  / n_cols;
  grid->cell_height = grid->widget.height / n_rows;

  grid->state        = CLTR_PHOTO_GRID_STATE_BROWSE;
  grid->is_populated = FALSE;

  grid->anim_fps     = 50;

  grid->anim_n_steps = 10; /* value needs to be calced dep on rows */
  grid->anim_step    = 0;

  /* Default 'browse view' */
  grid->zoom_min  = 1.0;		      
  grid->view_min_x = (grid->widget.width - (grid->zoom_min * grid->widget.width))/2.0;
  grid->view_min_y = 0.0;

  /* Assmes cols == rows */
  grid->zoom_max  = /* 1.0 + */  (float) (n_rows * 1.0); //  - 0.3;

  grid->row_offset = 0;

  grid->mutex = g_mutex_new();

  return CLTR_WIDGET(grid);
}
