/*
 * Copyright (C) 2003 Robert Kooima
 *
 * NEVERPUTT is  free software; you can redistribute  it and/or modify
 * it under the  terms of the GNU General  Public License as published
 * by the Free  Software Foundation; either version 2  of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT  ANY  WARRANTY;  without   even  the  implied  warranty  of
 * MERCHANTABILITY or  FITNESS FOR A PARTICULAR PURPOSE.   See the GNU
 * General Public License for more details.
 */

#include <SDL.h>
#include <math.h>

#include "glext.h"
#include "game.h"
#include "vec3.h"
#include "geom.h"
#include "ball.h"
#include "back.h"
#include "hole.h"
#include "hud.h"
#include "image.h"
#include "audio.h"
#include "solid_gl.h"
#include "solid_phys.h"
#include "config.h"

#define TARGET_DISTANCE 0.1f
#define TARGET_DISTANCE_NEAR 15.f
#define TARGET_SPEED 4.f
#define TARGET_ACCELERATION 0.0005f  /* higher value more acceleration */
#define TARGET_V_FACTOR 10.f

/*---------------------------------------------------------------------------*/

static struct      s_file file;
static int                ball;
static int current_view_target;
static int    last_view_target;

static float view_a;                    /* Ideal view rotation about Y axis  */
static float view_m;
static float view_ry;                   /* Angular velocity about Y axis     */
static float view_dy;                   /* Ideal view distance above ball    */
static float view_dz;                   /* Ideal view distance behind ball   */
static float view_target_dt;            /* How far the camera's traveled     */

static float view_c[3];                 /* Current view center               */
static float view_v[3];                 /* Current view vector               */
static float view_p[3];                 /* Current view position             */
static float view_e[3][3];              /* Current view orientation          */
static float view_vlt[3];               /* Last target's view vector         */

static int   jump_s;                    /* Has ball reached destination?     */
static int   jump_u;                    /* Which ball is jumping?            */
static int   jump_b;                    /* Jump-in-progress flag             */
static float jump_dt;                   /* Jump duration                     */
static float jump_p[3];                 /* Jump destination                  */

/*---------------------------------------------------------------------------*/

static void view_init(void)
{
    view_a  = 0.f;
    view_m  = 0.f;
    view_ry = 0.f;
    view_dy = 3.f;
    view_dz = 5.f;

    view_c[0] = 0.f;
    view_c[1] = 0.f;
    view_c[2] = 0.f;

    view_p[0] =     0.f;
    view_p[1] = view_dy;
    view_p[2] = view_dz;

    view_e[0][0] = 1.f;
    view_e[0][1] = 0.f;
    view_e[0][2] = 0.f;
    view_e[1][0] = 0.f;
    view_e[1][1] = 1.f;
    view_e[1][2] = 0.f;
    view_e[2][0] = 0.f;
    view_e[2][1] = 0.f;
    view_e[2][2] = 1.f;
}

void game_init(const char *s)
{
    jump_s  = 1;
    jump_u  = 0;
    jump_b  = 0;
    jump_dt = 0.f;
    view_target_dt = 0.f;

    view_init();
    sol_load_gl(&file, config_data(s), config_get_d(CONFIG_TEXTURES),
                                    config_get_d(CONFIG_SHADOW));

    game_ball_inactivate(BALL_ALL);
}

void game_free(void)
{
    sol_free_gl(&file);
}

/*---------------------------------------------------------------------------*/

static void game_draw_vect_prim(const struct s_file *fp, GLenum mode)
{
    float p[3];
    float x[3];
    float z[3];
    float r;

    v_cpy(p, fp->uv[abs(current_view_target)].p);
    v_cpy(x, view_e[0]);
    v_cpy(z, view_e[2]);

    r = fp->uv[abs(current_view_target)].r;

    glBegin(mode);
    {
        glColor4f(1.0f, 1.0f, 0.5f, 0.5f);
        glVertex3f(p[0] - x[0] * r,
                   p[1] - x[1] * r,
                   p[2] - x[2] * r);

        glColor4f(1.0f, 0.0f, 0.0f, 0.5f);
        glVertex3f(p[0] + z[0] * view_m,
                   p[1] + z[1] * view_m,
                   p[2] + z[2] * view_m);

        glColor4f(1.0f, 1.0f, 0.0f, 0.5f);
        glVertex3f(p[0] + x[0] * r,
                   p[1] + x[1] * r,
                   p[2] + x[2] * r);
    }
    glEnd();
}

static void game_draw_vect(const struct s_file *fp)
{
    if (view_m > 0.f)
    {
        glPushAttrib(GL_TEXTURE_BIT);
        glPushAttrib(GL_POLYGON_BIT);
        glPushAttrib(GL_LIGHTING_BIT);
        glPushAttrib(GL_DEPTH_BUFFER_BIT);
        {
            glEnable(GL_COLOR_MATERIAL);
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glDepthMask(GL_FALSE);

            glEnable(GL_DEPTH_TEST);
            game_draw_vect_prim(fp, GL_TRIANGLES);

            glDisable(GL_DEPTH_TEST);
            game_draw_vect_prim(fp, GL_LINE_STRIP);
        }
        glPopAttrib();
        glPopAttrib();
        glPopAttrib();
        glPopAttrib();
    }
}

static void game_draw_balls(const struct s_file *fp,
                            const float *bill_M, float t)
{
    static const GLfloat color[5][4] = {
        { 1.0f, 1.0f, 1.0f, 0.7f },
        { 1.0f, 0.0f, 0.0f, 1.0f },
        { 0.0f, 1.0f, 0.0f, 1.0f },
        { 0.0f, 0.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 0.0f, 1.0f },
    };

    int ui;

    for (ui = 1; ui < fp->uc; ui++)
    {
        if (fp->uv[ui].a)
        {
            float ball_M[16];
            float pend_M[16];

            m_basis(ball_M, fp->uv[ui].e[0], fp->uv[ui].e[1], fp->uv[ui].e[2]);
            m_basis(pend_M, fp->uv[ui].E[0], fp->uv[ui].E[1], fp->uv[ui].E[2]);

            glPushMatrix();
            {
                glTranslatef(fp->uv[ui].p[0],
                             fp->uv[ui].p[1] + BALL_FUDGE,
                             fp->uv[ui].p[2]);
                glScalef(fp->uv[ui].r,
                         fp->uv[ui].r,
                         fp->uv[ui].r);

                glEnable(GL_COLOR_MATERIAL);
                glColor4fv(color[ui]);
                ball_draw(ball_M, pend_M, bill_M, t);
                glDisable(GL_COLOR_MATERIAL);
            }
            glPopMatrix();
        }
        else if (ui <= curr_party())
        {
            glPushMatrix();
            {
                glTranslatef(fp->uv[ui].p[0],
                             fp->uv[ui].p[1] - fp->uv[ui].r + BALL_FUDGE,
                             fp->uv[ui].p[2]);
                glScalef(fp->uv[ui].r,
                         fp->uv[ui].r,
                         fp->uv[ui].r);

                glColor4f(color[ui][0],
                          color[ui][1],
                          color[ui][2], 0.5f);

                mark_draw();
            }
            glPopMatrix();
        }
    }
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

static void game_draw_goals(const struct s_file *fp)
{
    int zi;

    for (zi = 0; zi < fp->zc; zi++)
    {
        glPushMatrix();
        {
            glTranslatef(fp->zv[zi].p[0],
                         fp->zv[zi].p[1],
                         fp->zv[zi].p[2]);
            flag_draw();
        }
        glPopMatrix();
    }
}

static void game_draw_jumps(const struct s_file *fp)
{
    int ji;

    for (ji = 0; ji < fp->jc; ji++)
    {
        glPushMatrix();
        {
            glTranslatef(fp->jv[ji].p[0],
                         fp->jv[ji].p[1],
                         fp->jv[ji].p[2]);

            glScalef(fp->jv[ji].r, 1.f, fp->jv[ji].r);
            jump_draw(fp->jv[ji].b ? 1 : 0);
        }
        glPopMatrix();
    }
}

static void game_draw_swchs(const struct s_file *fp)
{
    int xi;

    for (xi = 0; xi < fp->xc; xi++)
    {
        if (fp->xv[xi].i)
            continue;

        glPushMatrix();
        {
            glTranslatef(fp->xv[xi].p[0],
                         fp->xv[xi].p[1],
                         fp->xv[xi].p[2]);

            glScalef(fp->xv[xi].r, 1.f, fp->xv[xi].r);
            swch_draw(fp->xv[xi].f, fp->xv[xi].e);
        }
        glPopMatrix();
    }
}

/*---------------------------------------------------------------------------*/

void game_draw(int pose, float t)
{
    static const float a[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    static const float s[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    static const float e[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    static const float h[1] = { 0.0f };

    const float light_p[4] = { 8.f, 32.f, 8.f, 1.f };

    const struct s_file *fp = &file;

    float fov = FOV;

    int i = 0;

    if (jump_b && jump_u == abs(current_view_target))
    {
        fov *= 2.0f * fabsf(jump_dt - 0.5f);
        current_view_target = last_view_target = abs(current_view_target);
        v_cpy(view_vlt, view_v);
    }

    config_push_persp(fov, 0.1f, FAR_DIST);
    glPushAttrib(GL_LIGHTING_BIT);
    glPushMatrix();
    {
        float T[16], M[16], v[3], rx, ry;

        m_view(T, view_c, view_p, view_e[1]);
        m_xps(M, T);

        v_sub(v, view_c, view_p);

        rx = V_DEG(fatan2f(-v[1], fsqrtf(v[0] * v[0] + v[2] * v[2])));
        ry = V_DEG(fatan2f(+v[0], -v[2]));

        glTranslatef(0.f, 0.f, -v_len(v));
        glMultMatrixf(M);
        glTranslatef(-view_c[0], -view_c[1], -view_c[2]);

        /* Center the skybox about the position of the camera. */

        glPushMatrix();
        {
            glTranslatef(view_p[0], view_p[1], view_p[2]);
            back_draw(0);
        }
        glPopMatrix();

        glEnable(GL_LIGHT0);
        glLightfv(GL_LIGHT0, GL_POSITION, light_p);

        /* Draw the floor. */

        sol_draw(fp, 0, 1);

        if (config_get_d(CONFIG_SHADOW) && !pose)
        {
            for (i = 0; i < fp->uc; i++)
            {
                if (fp->uv[i].a)
                {
                    shad_draw_set(fp->uv[i].p, fp->uv[i].r);
                    sol_shad(fp);
                    shad_draw_clr();
                }
            }
        }

        /* Draw the game elements. */

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        if (pose == 0)
        {
            game_draw_balls(fp, T, t);
            game_draw_vect(fp);
        }

        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT,   a);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR,  s);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION,  e);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, h);

        game_draw_goals(fp);

        glEnable(GL_COLOR_MATERIAL);
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDepthMask(GL_FALSE);
        {
            game_draw_jumps(fp);
            game_draw_swchs(fp);
        }
        glDepthMask(GL_TRUE);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_LIGHTING);
        glDisable(GL_COLOR_MATERIAL);
    }
    glPopMatrix();
    glPopAttrib();
    config_pop_matrix();
}

/*---------------------------------------------------------------------------*/

void game_update_view(float dt)
{
    struct s_ball *up;

    const float y[3] = { 0.f, 1.f, 0.f };

    float dy;
    float dz;
    float k;
    float e[3];
    float d[3];
    float tmp[3];
    float s = 2.f * dt;
    float l = 1.0e+5f;
    float pr;
    int i;

    /* Center the view about the ball. */

    up = &file.uv[ball];
    if (up->a && (!config_get_d(CONFIG_PUTT_COLLISIONS) || v_len(up->v) - dt > 0.0005f))
    {
        if (current_view_target == ball)
        {
            v_cpy(view_c, up->p);
            v_inv(view_v, up->v);
        }
        else
        {
            current_view_target = -1 * ball;  /* en route */

            v_sub(d, file.uv[last_view_target >= 0 ? last_view_target : 0].p, view_c);
            if (last_view_target < 0 || v_len(d) > file.uv[last_view_target].r * TARGET_DISTANCE_NEAR)
            {
                last_view_target = -1 * ball;
                v_sub(tmp, up->p, view_c);
                v_mad(view_c, view_c, tmp, (view_target_dt += dt * TARGET_SPEED * TARGET_ACCELERATION));
                v_sub(tmp, file.uv[last_view_target].p, view_c);
                pr = v_len(tmp);
                v_sub(tmp, file.uv[last_view_target].p, file.uv[current_view_target].p);
                pr = v_len(tmp) / pr;
                v_sub(tmp, view_vlt, file.uv[current_view_target].v);
                v_mad(view_v, view_vlt, tmp, pr);
            }
            else
            {
                v_sub(tmp, up->p, view_c);
                v_mad(view_c, view_c, tmp, dt * TARGET_SPEED);
                v_sub(tmp, file.uv[last_view_target].p, view_c);
                pr = v_len(tmp);
                v_sub(tmp, file.uv[last_view_target].p, file.uv[current_view_target].p);
                pr = v_len(tmp) / pr;
                v_sub(tmp, view_vlt, file.uv[current_view_target].v);
                v_mad(view_v, view_vlt, tmp, pr);
            }

            v_sub(d, up->p, view_c);
            if (v_len(d) < up->r * TARGET_DISTANCE)
            {
                current_view_target = last_view_target = ball;
                view_target_dt = 0.f;
                v_cpy(view_vlt, view_v);
            }
        }
    }
    else
    {
        /*
         * the current ball has stopped moving, so use the nearest active,
         * moving ball if there is one
         */

        for (i = 1; i < file.uc; i++)
        {
            up = &file.uv[i];

            if (!up->a)
                continue;

            if (v_len(up->v) - dt <= 0.0005f)
                continue;

            v_sub(d, up->p, view_c);
            if (v_len(d) < l)
            {
                if (current_view_target == i)
                {
                    v_cpy(view_c, up->p);
                    v_inv(view_v, up->v);
                }
                else
                {
                    current_view_target = -1 * i;  /* en route */

                    v_sub(d, file.uv[last_view_target >= 0 ? last_view_target : 0].p, view_c);
                    if (last_view_target < 0 || v_len(d) > file.uv[last_view_target].r * TARGET_DISTANCE_NEAR)
                    {
                        last_view_target = -1 * i;
                        v_sub(tmp, up->p, view_c);
                        v_mad(view_c, view_c, tmp, (view_target_dt += dt * TARGET_SPEED * TARGET_ACCELERATION));
                        v_sub(tmp, file.uv[last_view_target].p, view_c);
                        pr = v_len(tmp);
                        v_sub(tmp, file.uv[last_view_target].p, file.uv[current_view_target].p);
                        pr = v_len(tmp) / pr;
                        v_sub(tmp, view_vlt, file.uv[current_view_target].v);
                        v_mad(view_v, view_vlt, tmp, pr);
                    }
                    else
                    {
                        v_sub(tmp, up->p, view_c);
                        v_mad(view_c, view_c, tmp, dt * TARGET_SPEED);
                        v_sub(tmp, file.uv[last_view_target].p, view_c);
                        pr = v_len(tmp);
                        v_sub(tmp, file.uv[last_view_target].p, file.uv[current_view_target].p);
                        pr = v_len(tmp) / pr;
                        v_sub(tmp, view_vlt, file.uv[current_view_target].v);
                        v_mad(view_v, view_vlt, tmp, pr);
                    }

                    v_sub(d, up->p, view_c);
                    if (v_len(d) < up->r * TARGET_DISTANCE)
                    {
                        current_view_target = last_view_target = i;
                        view_target_dt = 0.f;
                        v_cpy(view_vlt, view_v);
                    }
                }
            }
        }
    }

    switch (config_get_d(CONFIG_CAMERA))
    {
    case 2:
        /* Camera 2: View vector is given by view angle. */

        view_e[2][0] = fsinf(V_RAD(view_a));
        view_e[2][1] = 0.f;
        view_e[2][2] = fcosf(V_RAD(view_a));

        s = 1.f;
        break;

    default:
        /* View vector approaches the ball velocity vector. */

        v_mad(e, view_v, y, v_dot(view_v, y));
        v_inv(e, e);

        k = v_dot(view_v, view_v);

        v_sub(view_e[2], view_p, view_c);
        v_mad(view_e[2], view_e[2], view_v, k * dt * 0.1f);
    }

    /* Orthonormalize the basis of the view in its new position. */

    v_crs(view_e[0], view_e[1], view_e[2]);
    v_crs(view_e[2], view_e[0], view_e[1]);
    v_nrm(view_e[0], view_e[0]);
    v_nrm(view_e[2], view_e[2]);

    /* The current view (dy, dz) approaches the ideal (view_dy, view_dz). */

    v_sub(d, view_p, view_c);

    dy = v_dot(view_e[1], d);
    dz = v_dot(view_e[2], d);

    dy += (view_dy - dy) * s;
    dz += (view_dz - dz) * s;

    /* Compute the new view position. */

    view_p[0] = view_p[1] = view_p[2] = 0.f;

    v_mad(view_p, view_c, view_e[1], dy);
    v_mad(view_p, view_p, view_e[2], dz);

    view_a = V_DEG(fatan2f(view_e[2][0], view_e[2][2]));
}

static int game_update_state(float dt)
{
    struct s_file *fp = &file;

    static float t = 0.f;
    float p[3], d[3], z[3] = {0.f, 0.f, 0.f};
    int i, j, u, ui, m = 0, c = 0;

    if (dt > 0.f)
        t += dt;
    else
        t = 0.f;

    if (fp->uv[ball].a && v_len(fp->uv[ball].v) - dt > 0.f)
        c = 1; /* the current ball is in motion */
    for (ui = 1; ui < fp->uc; ui++)
        if (ui != ball && fp->uv[ui].a && v_len(fp->uv[ui].v) - dt > 0.f)
            m = 1; /* a non-current ball is in motion */

    /* Test for a switch. */

    if (sol_swch_test(fp))
        audio_play(AUD_SWITCH, 1.f);

    /* Test for a jump. */

    if (!jump_b && (u = sol_jump_test(fp, jump_p)))
    {
        jump_b = 1;
        jump_u = u - 1;

        audio_play(AUD_JUMP, 1.f);
    }

    /* Test for fall-out. */

    for (ui = 1; ui < fp->uc; ui++)
    {
        if (ui != ball && fp->uv[ui].a && fp->uv[ui].p[1] < -10.f)
        {
            game_ball_inactivate(ui);
            v_cpy(fp->uv[ui].v, z);
            v_cpy(fp->uv[ui].w, z);
            hole_fall(ui);
        }
    }

    if (!m && fp->uv[ball].p[1] < -10.f)
    {
        game_ball_inactivate(ball);
        v_cpy(fp->uv[ball].v, z);
        v_cpy(fp->uv[ball].w, z);
        return GAME_FALL;
    }

    /* Test for intersections */

    for (i = 0; i < fp->uc; i++)
    {
        struct s_ball *up = fp->uv + i;

        if (!up->a || v_len(up->v) > 0.f)
            continue;

        for (j = i + 1; j < fp->uc; j++)
        {
            struct s_ball *u2p = fp->uv + j;

            if (!u2p->a || v_len(u2p->v) > 0.f)
                continue;

            v_sub(d, up->p, u2p->p);

            if (v_len(d) * 1.1f < up->r + u2p->r)
            {
                if(i == ball)
                    game_ball_inactivate(j);
                else
                    game_ball_inactivate(i);
            }
        }
    }

    /* Test for a goal or stop. */

    for (ui = 1; ui < fp->uc; ui++)
    {
        if (ui != ball && fp->uv[ui].a && !(v_len(fp->uv[ui].v) > 0.0f) && sol_goal_test(fp, p, ui))
        {
            game_ball_inactivate(ui);
            hole_goal(ui);
        }
    }

    if (!m && !c && t > 1.f)
    {
        t = 0.f;

        if (sol_goal_test(fp, p, ball))
        {
            game_ball_inactivate(ball);
            return GAME_GOAL;
        }
        else
        {
            return GAME_STOP;
        }
    }

    return GAME_NONE;
}

/*
 * On  most  hardware, rendering  requires  much  more  computing power  than
 * physics.  Since  physics takes less time  than graphics, it  make sense to
 * detach  the physics update  time step  from the  graphics frame  rate.  By
 * performing multiple physics updates for  each graphics update, we get away
 * with higher quality physics with little impact on overall performance.
 *
 * Toward this  end, we establish a  baseline maximum physics  time step.  If
 * the measured  frame time  exceeds this  maximum, we cut  the time  step in
 * half, and  do two updates.  If THIS  time step exceeds the  maximum, we do
 * four updates.  And  so on.  In this way, the physics  system is allowed to
 * seek an optimal update rate independent of, yet in integral sync with, the
 * graphics frame rate.
 */

int game_step(const float g[3], float dt)
{
    struct s_file *fp = &file;

    static float s = 0.f;
    static float t = 0.f;

    float d = 0.f;
    float b = 0.f;
    float st = 0.f;
    int i, n = 1, m = 0;

    s = (7.f * s + dt) / 8.f;
    t = s;

    if( !jump_b ) /* one ball at a time */
    {
        /* Run the sim. */

        while (t > MAX_DT && n < MAX_DN)
        {
            t /= 2;
            n *= 2;
        }

        for (i = 0; i < n; i++)
        {
            d = sol_step(fp, g, t, ball, &m);

            if (b < d)
                b = d;
            if (!m)
                st += t;
        }

        /* Mix the sound of a ball bounce. */

        if (b > 0.5)
            audio_play(AUD_BUMP, (float) (b - 0.5) * 2.0f);
    }
    else
    {
        /* Handle a jump. */

        jump_dt += dt;

        if (0.5f < jump_dt && jump_s)
        {
            jump_s = 0;
            fp->uv[jump_u].p[0] = jump_p[0];
            fp->uv[jump_u].p[1] = jump_p[1];
            fp->uv[jump_u].p[2] = jump_p[2];
            sol_jump_test(fp, NULL);
        }

        if (1.f  < jump_dt)
        {
            jump_dt = 0.f;
            jump_b  = 0;
            jump_s  = 1;
        }
    }

    game_update_view(dt);
    return game_update_state(st);
}

void game_putt(void)
{
    /*
     * HACK: The BALL_FUDGE here  guarantees that a putt doesn't drive
     * the ball  too directly down  toward a lump,  triggering rolling
     * friction too early and stopping the ball prematurely.
     */

    file.uv[abs(current_view_target)].v[0] = -4.f * view_e[2][0] * view_m;
    file.uv[abs(current_view_target)].v[1] = -4.f * view_e[2][1] * view_m + BALL_FUDGE;
    file.uv[abs(current_view_target)].v[2] = -4.f * view_e[2][2] * view_m;

    view_m = 0.f;
}

/*---------------------------------------------------------------------------*/

void game_ball_activate(int b)
{
    int i;

    if (b == BALL_CURRENT)
    {
        file.uv[ball].a = 1;
    }
    else if (b == BALL_PARTY)
    {
        for (i = 1; i <= curr_party(); i++)
        {
            file.uv[i].a = 1;
        }
    }
    else if (b == BALL_ALL)
    {
        for (i = 1; i < file.uc; i++)
        {
            file.uv[i].a = 1;
        }
    }
    else
    {
        file.uv[b].a = 1;
    }
}

void game_ball_inactivate(int b)
{
    int i;

    if (b == BALL_CURRENT)
    {
        file.uv[ball].a = 0;
    }
    else if (b == BALL_PARTY)
    {
        for (i = 1; i <= curr_party(); i++)
        {
            file.uv[i].a = 0;
        }
    }
    else if (b == BALL_ALL)
    {
        for (i = 1; i < file.uc; i++)
        {
            file.uv[i].a = 0;
        }
    }
    else
    {
        file.uv[b].a = 0;
    }
}

/*---------------------------------------------------------------------------*/

void game_set_rot(int d)
{
    view_a += (float) (30.f * d) / config_get_d(CONFIG_MOUSE_SENSE);
}

void game_clr_mag(void)
{
    view_m = 1.f;
}

void game_set_mag(int d)
{
    view_m -= (float) (1.f * d) / config_get_d(CONFIG_MOUSE_SENSE);

    if (view_m < 0.25)
        view_m = 0.25;
}

void game_set_fly(float k)
{
    struct s_file *fp = &file;

    float  x[3] = { 1.f, 0.f, 0.f };
    float  y[3] = { 0.f, 1.f, 0.f };
    float  z[3] = { 0.f, 0.f, 1.f };
    float c0[3] = { 0.f, 0.f, 0.f };
    float p0[3] = { 0.f, 0.f, 0.f };
    float c1[3] = { 0.f, 0.f, 0.f };
    float p1[3] = { 0.f, 0.f, 0.f };
    float  v[3];

    v_cpy(view_e[0], x);
    v_cpy(view_e[1], y);
    v_sub(view_e[2], fp->uv[ball].p, fp->zv[0].p);

    if (fabs(v_dot(view_e[1], view_e[2])) > 0.999)
        v_cpy(view_e[2], z);

    v_crs(view_e[0], view_e[1], view_e[2]);
    v_crs(view_e[2], view_e[0], view_e[1]);

    v_nrm(view_e[0], view_e[0]);
    v_nrm(view_e[2], view_e[2]);

    /* k = 0.0 view is at the ball. */

    if (fp->uc > 0)
    {
        v_cpy(c0, fp->uv[ball].p);
        v_cpy(p0, fp->uv[ball].p);
    }

    v_mad(p0, p0, view_e[1], view_dy);
    v_mad(p0, p0, view_e[2], view_dz);

    /* k = +1.0 view is s_view 0 */

    if (k >= 0 && fp->wc > 0)
    {
        v_cpy(p1, fp->wv[0].p);
        v_cpy(c1, fp->wv[0].q);
    }

    /* k = -1.0 view is s_view 1 */

    if (k <= 0 && fp->wc > 1)
    {
        v_cpy(p1, fp->wv[1].p);
        v_cpy(c1, fp->wv[1].q);
    }

    /* Interpolate the views. */

    v_sub(v, p1, p0);
    v_mad(view_p, p0, v, k * k);

    v_sub(v, c1, c0);
    v_mad(view_c, c0, v, k * k);

    /* Orthonormalize the view basis. */

    v_sub(view_e[2], view_p, view_c);
    v_crs(view_e[0], view_e[1], view_e[2]);
    v_crs(view_e[2], view_e[0], view_e[1]);
    v_nrm(view_e[0], view_e[0]);
    v_nrm(view_e[2], view_e[2]);

    view_a = V_DEG(fatan2f(view_e[2][0], view_e[2][2]));
}

void game_ball(int i)
{
    int ui;

    ball = current_view_target = last_view_target = i;
    view_target_dt = 0.f;
    v_cpy(view_vlt, view_v);

    for (ui = 0; ui < file.uc; ui++)
    {
        file.uv[ui].v[0] = 0.f;
        file.uv[ui].v[1] = 0.f;
        file.uv[ui].v[2] = 0.f;

        file.uv[ui].w[0] = 0.f;
        file.uv[ui].w[1] = 0.f;
        file.uv[ui].w[2] = 0.f;
    }
}

void game_get_pos(float p[3], float e[3][3], int ui)
{
    if( ui == BALL_CURRENT )
        ui = ball;

    v_cpy(p,    file.uv[ui].p);
    v_cpy(e[0], file.uv[ui].e[0]);
    v_cpy(e[1], file.uv[ui].e[1]);
    v_cpy(e[2], file.uv[ui].e[2]);
}

void game_set_pos(float p[3], float e[3][3], int ui)
{
    if( ui == BALL_CURRENT )
        ui = ball;

    v_cpy(file.uv[ui].p,    p);
    v_cpy(file.uv[ui].e[0], e[0]);
    v_cpy(file.uv[ui].e[1], e[1]);
    v_cpy(file.uv[ui].e[2], e[2]);
}

int game_get_aggressor(int ui)
{
    return file.uv[ui].g;
}

void game_set_aggressor(int ui, int val)
{
    int i;

    if (ui == BALL_CURRENT)
    {
        file.uv[ball].g = val;
    }
    else if (ui == BALL_PARTY)
    {
        for (i = 1; i <= curr_party(); i++)
        {
            file.uv[i].g = val;
        }
    }
    else if (ui == BALL_ALL)
    {
        for (i = 1; i < file.uc; i++)
        {
            file.uv[i].g = val;
        }
    }
    else
    {
        file.uv[ui].g = val;
    }
}

/*---------------------------------------------------------------------------*/

