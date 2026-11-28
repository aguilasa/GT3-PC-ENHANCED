/* pc_racing_line.h - PC: the racing line on the road, a thin strip ahead of
 * the car of the view.  No original: new PC code; Linux only (the whole
 * module is left out of the Windows build).
 *
 * The strip follows line 6 of the course's enemy lines (the racing line the
 * computer drivers follow, T368): kSamples points from 2 m to 120 m ahead
 * of the car's lap distance, 2 m apart, each found by line_point_at (T371)
 * from the node the previous one returned, turned from the lines' space
 * (x, y) into the view's (x, height, -y) at the road's height under it (the
 * runway query from the runway sector of the point before, else of its line
 * node: the line's own height, interpolated between its nodes, is a metre
 * off the road on a banked turn and metres on a hilly dirt course), and
 * widened into a left and a right vertex perpendicular to the way the strip
 * runs there, a few centimetres above the road.  A course whose line 6 is
 * empty (lines_prepare clears its slot) has no strip; line 6 is read
 * directly, never through lines_select (which would give line 1 or 2 in its
 * place).
 *
 * [Debug] RacingLineLog=on writes the strip into gt3pc.log: for every
 * kLogEvery-th frame of a view, a "racing line: frame" line (the car, its
 * lap distance, its view position, the ground under it and the road probed
 * there, the line point at its own distance, the first sample) and then one
 * "racing line: sample" line for each sample (its distance, its point, how
 * its road height was found, the line's own height there, the two
 * vertices); once for a course, line 6's nodes ("racing line: node").
 *
 * [Debug] RacingLineForce=on draws the strip (a stand-in for its own
 * setting): semi-transparent, one colour, fading in over its first metres
 * and out over its last, depth tested against the course and the cars but
 * not writing depth, blended into the colour only (the frame's alpha, which
 * the effects after the cars read, is kept).  Only in a live race: not in a
 * replay, a demo (the attract demo, the race behind a menu), a race preview
 * (the licence's demonstration), the logger, the debug camera's view; the
 * rear-view mirror (drawn by its own pass after it) has none. */
#ifndef GAME_APP_PC_RACING_LINE_H
#define GAME_APP_PC_RACING_LINE_H

#ifndef _WIN32

class RacePS2Base;
class CameraManager;

/* The strip for the current view of cm (its car: targetInfo.car[0]), after
 * the cars of RacePS2Base::drawView (T131).  Returns at once when nothing
 * asks for it (the log off, the strip off or not shown in this view) and on
 * a course without line 6. */
void pc_racing_line_draw(RacePS2Base *race, CameraManager *cm);

#endif /* !_WIN32 */

#endif /* GAME_APP_PC_RACING_LINE_H */
