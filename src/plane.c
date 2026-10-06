/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read a list of people who contributed.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

#include "vim.h"

/*
 * Create a new screen plane with the given size. If "old" is not NULL, then
 * update references and copy attributes over. Note that "old" will be freed if
 * so. Returns NULL on failure.
 */
    static sc_plane_T *
sc_plane_new(sc_plane_T *old, int rows, int cols)
{
    sc_plane_T *plane = alloc_clear(sizeof(*plane) + (rows * cols * sizeof(sc_cell_T)) - sizeof(sc_cell_T));

    if (plane == NULL)
	return NULL;

    plane->rows = rows;
    plane->cols = cols;

    if (old != NULL)
    {
	sc_plane_T  *child = old->child;
	int	    common_rows, common_cols;

	plane->row = old->row;
	plane->col = old->col;

	if (old->parent != NULL && old->parent->child == old)
	    old->parent->child = plane;

	while (child != NULL)
	{
	    child->parent = plane;
	    child = child->next;
	}

	if (old->prev != NULL)
	    old->prev->next = plane;
	if (old->next != NULL)
	    old->next->prev = plane;

	common_rows = MIN(rows, old->rows);
	common_cols = MIN(cols, old->cols);

	// Copy over cells
	for (int r = 0; r < rows; r++)
	{
	    for (int c = 0; c < cols; c++)
	    {
		if (r >= common_rows || c >= common_cols)
		    // Invalidate cells that are not copied over (if plane is
		    // bigger now).
		    plane->cells[r * cols + c].attr = -1;
		else
		    plane->cells[r * cols + c] = old->cells[r * cols + c];
	    }
	}
    }
    else
    {
	// Invalidate all cells
	for (int r = 0; r < rows; r++)
	    for (int c = 0; c < cols; c++)
		plane->cells[r * cols + c].attr = -1;
    }

    return plane;
}

/*
 * Add "plane" to children of "parent", adding it in the correct spot according
 * to zindex.
 */
    static void
sc_plane_add_child(sc_plane_T *parent, sc_plane_T *plane)
{
    sc_plane_T *child = parent->child;

    while (child != NULL)
    {
	if (child->zindex > plane->zindex)
	    break;
	child = child->next;
    }

    if (child == parent->child)
    {
	parent->child = plane;
	plane->prev = NULL;
    }
    else
	plane->prev = child->prev;
    plane->next = child;
}

/*
 * Add a new screen plane with the given parent (may be NULL if toplevel) and
 * initial position + dimensions. Returns NULL on failure.
 */
    sc_plane_T *
sc_plane_add(sc_plane_T *parent, int row, int col, int rows, int cols)
{
    sc_plane_T *plane = sc_plane_new(NULL, rows, cols);

    if (plane == NULL)
	return NULL;


    plane->row = row;
    plane->col = col;

    if (parent != NULL)
	sc_plane_add_child(parent, plane);
    
    return plane;
}

/*
 * Composite all child planes onto "plane". Returns OK on success and FAIL on
 * failure.
 */
    void
sc_plane_composite(sc_plane_T *plane)
{
    sc_plane_T *child = plane->child;
    
    while (child != NULL)
    {
	sc_plane_composite(child);

	for (int r = 0; r < child->rows; r++)
	{
	    if (r + child->row >= plane->rows)
		break;

	    for (int c = 0; c < child->cols; c++)
	    {
		sc_cell_T   *cell;
		sc_cell_T   *ccell;
		int	    blend = 0;

		if (c + child->col >= plane->cols)
		    break;

		cell = plane->cells + (((r + child->row) * plane->cols) + c + child->col);
		ccell = child->cells + ((r * child->cols) + c);

		if (ccell->attr == (sattr_T)-1)
		    // Child cell is invalid
		    continue;

		if (child->transparency != NULL)
		    blend = child->transparency[r * child->cols + c];

		cell->attr = hl_blend_cell_attr(cell->attr, ccell->attr, blend);
		cell->codepoint = ccell->codepoint;
	    }
	}

	child = child->next;
    }
}

    void
sc_plane_emit(sc_plane_T *new, sc_plane_T
