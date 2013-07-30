#include "engine.h"

bool BIH::triintersect(const tri &t, const vec &o, const vec &ray, float maxdist, float &dist, int mode, const tri *noclip)
{
    vec p;
    p.cross(ray, t.c);
    float det = t.b.dot(p);
    if(det == 0) return false;
    vec r(o);
    r.sub(t.a);
    float u = r.dot(p) / det;
    if(u < 0 || u > 1) return false;
    vec q;
    q.cross(r, t.b);
    float v = ray.dot(q) / det;
    if(v < 0 || u + v > 1) return false;
    float f = t.c.dot(q) / det;
    if(f < 0 || f > maxdist) return false;
    if(!(mode&RAY_SHADOW) && &t >= noclip) return false;
    if(t.tex && (mode&RAY_ALPHAPOLY)==RAY_ALPHAPOLY && (t.tex->alphamask || (loadalphamask(t.tex), t.tex->alphamask)))
    {
        int si = clamp(int(t.tex->xs * (t.tc[0] + u*(t.tc[2] - t.tc[0]) + v*(t.tc[4] - t.tc[0]))), 0, t.tex->xs-1),
            ti = clamp(int(t.tex->ys * (t.tc[1] + u*(t.tc[3] - t.tc[1]) + v*(t.tc[5] - t.tc[1]))), 0, t.tex->ys-1);
        if(!(t.tex->alphamask[ti*((t.tex->xs+7)/8) + si/8] & (1<<(si%8)))) return false;
    }
    dist = f;
    return true;
}

struct BIHStack
{
    BIHNode *node;
    float tmin, tmax;
};

inline bool BIH::traverse(const vec &o, const vec &ray, const vec &invray, float maxdist, float &dist, int mode, BIHNode *curnode, float tmin, float tmax)
{
    BIHStack stack[128];
    int stacksize = 0;
    ivec order(ray.x>0 ? 0 : 1, ray.y>0 ? 0 : 1, ray.z>0 ? 0 : 1);
    for(;;)
    {
        int axis = curnode->axis();
        int nearidx = order[axis], faridx = nearidx^1;
        float nearsplit = (curnode->split[nearidx] - o[axis])*invray[axis],
              farsplit = (curnode->split[faridx] - o[axis])*invray[axis];

        if(nearsplit <= tmin)
        {
            if(farsplit < tmax)
            {
                if(!curnode->isleaf(faridx))
                {
                    curnode = &nodes[curnode->childindex(faridx)];
                    tmin = max(tmin, farsplit);
                    continue;
                }
                else if(triintersect(tris[curnode->childindex(faridx)], o, ray, maxdist, dist, mode, noclip)) return true;
            }
        }
        else if(curnode->isleaf(nearidx))
        {
            if(triintersect(tris[curnode->childindex(nearidx)], o, ray, maxdist, dist, mode, noclip)) return true;
            if(farsplit < tmax)
            {
                if(!curnode->isleaf(faridx))
                {
                    curnode = &nodes[curnode->childindex(faridx)];
                    tmin = max(tmin, farsplit);
                    continue;
                }
                else if(triintersect(tris[curnode->childindex(faridx)], o, ray, maxdist, dist, mode, noclip)) return true;
            }
        }
        else
        {
            if(farsplit < tmax)
            {
                if(!curnode->isleaf(faridx))
                {
                    if(stacksize < int(sizeof(stack)/sizeof(stack[0])))
                    {
                        BIHStack &save = stack[stacksize++];
                        save.node = &nodes[curnode->childindex(faridx)];
                        save.tmin = max(tmin, farsplit);
                        save.tmax = tmax;
                    }
                    else
                    {
                        if(traverse(o, ray, invray, maxdist, dist, mode, &nodes[curnode->childindex(nearidx)], tmin, min(tmax, nearsplit))) return true;
                        curnode = &nodes[curnode->childindex(faridx)];
                        tmin = max(tmin, farsplit);
                        continue;
                    }
                }
                else if(triintersect(tris[curnode->childindex(faridx)], o, ray, maxdist, dist, mode, noclip)) return true;
            }
            curnode = &nodes[curnode->childindex(nearidx)];
            tmax = min(tmax, nearsplit);
            continue;
        }
        if(stacksize <= 0) return false;
        BIHStack &restore = stack[--stacksize];
        curnode = restore.node;
        tmin = restore.tmin;
        tmax = restore.tmax;
    }
}

inline bool BIH::traverse(const vec &o, const vec &ray, float maxdist, float &dist, int mode)
{
    if(!numnodes) return false;

    vec invray(ray.x ? 1/ray.x : 1e16f, ray.y ? 1/ray.y : 1e16f, ray.z ? 1/ray.z : 1e16f);
    float tmin, tmax;
    float t1 = (bbmin.x - o.x)*invray.x,
          t2 = (bbmax.x - o.x)*invray.x;
    if(invray.x > 0) { tmin = t1; tmax = t2; } else { tmin = t2; tmax = t1; }
    t1 = (bbmin.y - o.y)*invray.y;
    t2 = (bbmax.y - o.y)*invray.y;
    if(invray.y > 0) { tmin = max(tmin, t1); tmax = min(tmax, t2); } else { tmin = max(tmin, t2); tmax = min(tmax, t1); }
    t1 = (bbmin.z - o.z)*invray.z;
    t2 = (bbmax.z - o.z)*invray.z;
    if(invray.z > 0) { tmin = max(tmin, t1); tmax = min(tmax, t2); } else { tmin = max(tmin, t2); tmax = min(tmax, t1); }
    if(tmin >= maxdist || tmin>=tmax) return false;
    tmax = min(tmax, maxdist);

    return traverse(o, ray, invray, maxdist, dist, mode, &nodes[0], tmin, tmax);
}

void BIH::build(vector<BIHNode> &buildnodes, ushort *indices, int numindices, const vec &vmin, const vec &vmax, int depth)
{
    maxdepth = max(maxdepth, depth);

    int axis = 2;
    loopk(2) if(vmax[k] - vmin[k] > vmax[axis] - vmin[axis]) axis = k;

    vec leftmin, leftmax, rightmin, rightmax;
    float splitleft, splitright;
    int left, right;
    loopk(3)
    {
        leftmin = rightmin = vec(1e16f, 1e16f, 1e16f);
        leftmax = rightmax = vec(-1e16f, -1e16f, -1e16f);
        float split = 0.5f*(vmax[axis] + vmin[axis]);
        for(left = 0, right = numindices, splitleft = SHRT_MIN, splitright = SHRT_MAX; left < right;)
        {
            tri &tri = tris[indices[left]];
            float amin = min(tri.a[axis], min(tri.b[axis], tri.c[axis])),
                  amax = max(tri.a[axis], max(tri.b[axis], tri.c[axis]));
            if(max(split - amin, 0.0f) > max(amax - split, 0.0f))
            {
                ++left;
                splitleft = max(splitleft, amax);
                leftmin.min(tri.a).min(tri.b).min(tri.c);
                leftmax.max(tri.a).max(tri.b).max(tri.c);
            }
            else
            {
                --right;
                swap(indices[left], indices[right]);
                splitright = min(splitright, amin);
                rightmin.min(tri.a).min(tri.b).min(tri.c);
                rightmax.max(tri.a).max(tri.b).max(tri.c);
            }
        }
        if(left > 0 && right < numindices) break;
        axis = (axis+1)%3;
    }

    if(!left || right==numindices)
    {
        leftmin = rightmin = vec(1e16f, 1e16f, 1e16f);
        leftmax = rightmax = vec(-1e16f, -1e16f, -1e16f);
        left = right = numindices/2;
        splitleft = SHRT_MIN;
        splitright = SHRT_MAX;
        loopi(numindices)
        {
            tri &tri = tris[indices[i]];
            if(i < left)
            {
                splitleft = max(splitleft, max(tri.a[axis], max(tri.b[axis], tri.c[axis])));
                leftmin.min(tri.a).min(tri.b).min(tri.c);
                leftmax.max(tri.a).max(tri.b).max(tri.c);
            }
            else
            {
                splitright = min(splitright, min(tri.a[axis], min(tri.b[axis], tri.c[axis])));
                rightmin.min(tri.a).min(tri.b).min(tri.c);
                rightmax.max(tri.a).max(tri.b).max(tri.c);
            }
        }
    }

    int node = buildnodes.length();
    buildnodes.add();
    buildnodes[node].split[0] = short(ceil(splitleft));
    buildnodes[node].split[1] = short(floor(splitright));

    if(left==1) buildnodes[node].child[0] = (axis<<14) | indices[0];
    else
    {
        buildnodes[node].child[0] = (axis<<14) | buildnodes.length();
        build(buildnodes, indices, left, leftmin, leftmax, depth+1);
    }

    if(numindices-right==1) buildnodes[node].child[1] = (1<<15) | (left==1 ? 1<<14 : 0) | indices[right];
    else
    {
        buildnodes[node].child[1] = (left==1 ? 1<<14 : 0) | buildnodes.length();
        build(buildnodes, &indices[right], numindices-right, rightmin, rightmax, depth+1);
    }
}

BIH::BIH(vector<tri> *t)
  : maxdepth(0), numnodes(0), nodes(NULL), numtris(0), tris(NULL), noclip(NULL), bbmin(1e16f, 1e16f, 1e16f), bbmax(-1e16f, -1e16f, -1e16f)
{
    numtris = t[0].length() + t[1].length();
    if(!numtris) return;

    tris = new tri[numtris];
    noclip = &tris[t[0].length()];
    memcpy(tris, t[0].getbuf(), t[0].length()*sizeof(tri));
    memcpy(noclip, t[1].getbuf(), t[1].length()*sizeof(tri));

    loopi(numtris)
    {
        tri &tri = tris[i];
        bbmin.min(tri.a).min(tri.b).min(tri.c);
        bbmax.max(tri.a).max(tri.b).max(tri.c);
    }

    radius = vec(bbmin).abs().max(vec(bbmax).abs()).magnitude();

    vector<BIHNode> buildnodes;
    ushort *indices = new ushort[numtris];
    loopi(numtris) indices[i] = i;

    maxdepth = 0;

    build(buildnodes, indices, numtris, bbmin, bbmax);

    delete[] indices;

    numnodes = buildnodes.length();
    nodes = new BIHNode[numnodes];
    memcpy(nodes, buildnodes.getbuf(), numnodes*sizeof(BIHNode));

    // convert tri.b/tri.c to edges
    loopi(numtris)
    {
        tri &tri = tris[i];
        tri.b.sub(tri.a);
        tri.c.sub(tri.a);
    }
}

bool mmintersect(const extentity &e, const vec &o, const vec &ray, float maxdist, int mode, float &dist)
{
    model *m = loadmapmodel(e.attr1);
    if(!m) return false;
    if(mode&RAY_SHADOW)
    {
        if(!m->shadow || e.flags&EF_NOSHADOW) return false;
    }
    else if((mode&RAY_ENTS)!=RAY_ENTS && (!m->collide || e.flags&EF_NOCOLLIDE)) return false;
    if(!m->bih && !m->setBIH()) return false;
    vec mo = vec(o).sub(e.o), mray(ray);
    int scale = e.attr5;
    if(scale > 0) mo.mul(100.0f/scale);
    float v = mo.dot(mray), inside = m->bih->radius*m->bih->radius - mo.squaredlen();
    if((inside < 0 && v > 0) || inside + v*v < 0) return false;
    int yaw = e.attr2, pitch = e.attr3, roll = e.attr4;
    if(yaw != 0)
    {
        const vec2 &rot = sincosmod360(-yaw);
        mo.rotate_around_z(rot);
        mray.rotate_around_z(rot);
    }
    if(pitch != 0)
    {
        const vec2 &rot = sincosmod360(-pitch);
        mo.rotate_around_x(rot);
        mray.rotate_around_x(rot);
    }
    if(roll != 0)
    {
        const vec2 &rot = sincosmod360(roll);
        mo.rotate_around_y(rot);
        mray.rotate_around_y(rot);
    }
    if(m->bih->traverse(mo, mray, maxdist ? maxdist : 1e16f, dist, mode))
    {
        if(scale > 0) dist *= scale/100.0f;
        return true;
    }
    return false;
}

static inline vec closestpointtri(const vec &p, const vec &a, const vec &ab, const vec &ac)
{
    vec ap = vec(p).sub(a);
    float d1 = ab.dot(ap), d2 = ac.dot(ap);
    if(d1 <= 0 && d2 <= 0) return ap;

    vec bp = vec(ap).sub(ab);
    float d3 = ab.dot(bp), d4 = ac.dot(bp);
    if(d3 >= 0 && d4 <= d3) return bp;

    float vc = d1*d4 - d3*d2;
    if(vc <= 0 && d1 >= 0 && d3 <= 0)
        return bp.sub(vec(ab).mul(d1 / (d1 - d3)));

    vec cp = vec(ap).sub(ac);
    float d5 = ab.dot(cp), d6 = ac.dot(cp);
    if(d6 >= 0 && d5 <= d6) return cp;

    float vb = d5*d2 - d1*d6;
    if(vb <= 0 && d2 >= 0 && d6 <= 0)
        return ap.sub(vec(ac).mul(d2 / (d2 - d6)));

    float va = d3*d6 - d5*d4;
    if(va <= 0 && d4 >= d3 && d5 >= d6)
        return bp.sub(vec(ac).sub(ab).mul((d4 - d3) / ((d4 - d3) + (d5 - d6))));

    float denom = 1 / (va + vb + vc);
    return ap.sub(vec(ab).mul(vb*denom).add(vec(ac).mul(vc*denom)));
}

static inline bool triboxoverlap(const vec &radius, const vec &a, const vec &ab, const vec &ac)
{
    vec b = vec(ab).add(a), c = vec(ac).add(a), 
        bc = vec(ac).sub(ab);

    #define TESTAXIS(v0, v1, v2, e, s, t) { \
        float p = v0.s*v1.t - v0.t*v1.s, \
              q = v2.s*e.t - v2.t*e.s, \
              r = radius.s*fabs(e.t) + radius.t*fabs(e.s); \
        if(p < q) { if(q < -r || p > r) return false; } \
        else if(p < -r || q > r) return false; \
    }
 
    TESTAXIS(a, b, c, ab, z, y);
    TESTAXIS(a, b, c, ab, x, z);
    TESTAXIS(a, b, c, ab, y, x);

    TESTAXIS(b, c, a, bc, z, y);
    TESTAXIS(b, c, a, bc, x, z);
    TESTAXIS(b, c, a, bc, y, x);

    TESTAXIS(a, c, b, ac, y, z);
    TESTAXIS(a, c, b, ac, z, x);
    TESTAXIS(a, c, b, ac, x, y); 

    #define TESTFACE(w) { \
        if(a.w < b.w) \
        { \
            if(b.w < c.w) { if(c.w < -radius.w || a.w > radius.w) return false; } \
            else if(b.w < -radius.w || min(a.w, c.w) > radius.w) return false; \
        } \
        else if(a.w < c.w) { if(c.w < -radius.w || b.w > radius.w) return false; } \
        else if(a.w < -radius.w || min(b.w, c.w) > radius.w) return false; \
    }

    TESTFACE(x);
    TESTFACE(y);
    TESTFACE(z);

    return true;
}

extern vec wall;
extern bool inside;

template<>
inline void BIH::tricollide<COLLIDE_ELLIPSE>(const tri &t, physent *d, const vec &dir, float cutoff, const vec &center, const vec &radius, const matrix3x3 &orient, float &dist, const vec &bo, const vec &br)
{
    vec a = orient.transform(t.a), b = orient.transform(t.b), c = orient.transform(t.c),
        p = closestpointtri(center, a, b, c).mul(radius);

    float pdist = p.squaredlen(), rdist = vec(p).mul(radius).squaredlen();
    if(pdist*pdist > rdist) return;

    pdist = sqrt(pdist) - sqrt(rdist/pdist);
    if(pdist <= dist) return;

    inside = true;

    vec n = vec().cross(b.mul(radius), c.mul(radius));
    float nmag = n.magnitude();

    if(!dir.iszero())
    {
        if(n.dot(dir) >= -cutoff*nmag) return;
        if(d->type==ENT_PLAYER &&
            pdist < (dir.z*n.z < 0 ?
               2*radius.z*(d->zmargin/(d->aboveeye+d->eyeheight)-(dir.z < 0 ? 1/3.0f : 1/4.0f)) :
               ((dir.x*n.x < 0 || dir.y*n.y < 0) ? -radius.x : 0)))
            return;
    }

    dist = pdist;
    wall = n.mul(1/nmag);
}

template<>
inline void BIH::tricollide<COLLIDE_OBB>(const tri &t, physent *d, const vec &dir, float cutoff, const vec &center, const vec &radius, const matrix3x4 &orient, float &dist, const vec &bo, const vec &br)
{
    vec a = orient.transform(t.a),
        b = orient.transformnormal(t.b),
        c = orient.transformnormal(t.c);
    if(!triboxoverlap(radius, a, b, c)) return;

    vec n;
    n.cross(b, c);
    float pdist = -n.dot(a), r = radius.absdot(n);
    if(fabs(pdist) > r) return;

    float nmag = 1/n.magnitude();
    pdist = (pdist - r)*nmag;
    if(pdist <= dist) return;
    n.mul(nmag);

    inside = true;

    if(!dir.iszero())
    {
        if(n.dot(dir) >= -cutoff) return;
        if(d->type==ENT_PLAYER &&
            pdist < (dir.z*n.z < 0 ?
               2*radius.z*(d->zmargin/(d->aboveeye+d->eyeheight)-(dir.z < 0 ? 1/3.0f : 1/4.0f)) :
               ((dir.x*n.x < 0 || dir.y*n.y < 0) ? -radius.x : 0)))
            return;
    }

    dist = pdist;
    wall = n;
}

template<int C, class M>
inline void BIH::collide(physent *d, const vec &dir, float cutoff, const vec &center, const vec &radius, const M &orient, float &dist, BIHNode *curnode, const vec &bo, const vec &br)
{
    BIHNode *stack[128];
    int stacksize = 0;
    vec bmin = vec(bo).sub(br), bmax = vec(bo).add(br);
    for(;;)
    {
        int axis = curnode->axis();
        int nearidx = 0, faridx = nearidx^1;
        float nearsplit = bmin[axis] - curnode->split[nearidx],
              farsplit = curnode->split[faridx] - bmax[axis];

        if(nearsplit > 0)
        {
            if(farsplit <= 0)
            {
                if(!curnode->isleaf(faridx))
                {
                    curnode = &nodes[curnode->childindex(faridx)];
                    continue;
                }
                else tricollide<C>(tris[curnode->childindex(faridx)], d, dir, cutoff, center, radius, orient, dist, bo, br);
            }
        }
        else if(curnode->isleaf(nearidx))
        {
            tricollide<C>(tris[curnode->childindex(nearidx)], d, dir, cutoff, center, radius, orient, dist, bo, br);
            if(farsplit <= 0)
            {
                if(!curnode->isleaf(faridx))
                {
                    curnode = &nodes[curnode->childindex(faridx)];
                    continue;
                }
                else tricollide<C>(tris[curnode->childindex(faridx)], d, dir, cutoff, center, radius, orient, dist, bo, br);
            }
        }
        else
        {
            if(farsplit <= 0)
            {
                if(!curnode->isleaf(faridx))
                {
                    if(stacksize < int(sizeof(stack)/sizeof(stack[0])))
                    {
                        stack[stacksize++] = &nodes[curnode->childindex(faridx)];
                    }
                    else
                    {
                        collide<C>(d, dir, cutoff, center, radius, orient, dist, &nodes[curnode->childindex(nearidx)], bo, br);
                        curnode = &nodes[curnode->childindex(faridx)];
                        continue;
                    }
                }
                else tricollide<C>(tris[curnode->childindex(faridx)], d, dir, cutoff, center, radius, orient, dist, bo, br);
            }
            curnode = &nodes[curnode->childindex(nearidx)];
            continue;
        }
        if(stacksize <= 0) return;
        curnode = stack[--stacksize];
    }
}

bool BIH::ellipsecollide(physent *d, const vec &dir, float cutoff, const vec &o, int yaw, int pitch, int roll, float scale)
{
    if(!numnodes) return false;

    vec center(d->o.x, d->o.y, d->o.z + 0.5f*(d->aboveeye - d->eyeheight)),
        radius(d->radius, d->radius, 0.5f*(d->eyeheight + d->aboveeye));
    center.sub(o);
    if(scale != 1) { scale = 1/scale; center.mul(scale); radius.mul(scale); }

    if(center.magnitude() >= radius.magnitude() + BIH::radius) return false;

    matrix3x3 orient;
    orient.identity();
    if(yaw) orient.rotate_around_z(sincosmod360(yaw));
    if(pitch) orient.rotate_around_x(sincosmod360(pitch));
    if(roll) orient.rotate_around_y(sincosmod360(-roll));

    vec bo = orient.transposedtransform(center), br = orient.abstransposedtransform(radius);
    if(bo.x + br.x < bbmin.x || bo.y + br.y < bbmin.y || bo.z + br.z < bbmin.z ||
       bo.x - br.x > bbmax.x || bo.y - br.y > bbmax.y || bo.z - br.z > bbmax.z)
        return false;

    vec invradius(1/radius.x, 1/radius.y, 1/radius.z);
    orient.a.mul(invradius.x);
    orient.b.mul(invradius.y);
    orient.c.mul(invradius.z);
   
    wall = vec(0, 0, 0);
    float dist = -1e10f;
    collide<COLLIDE_ELLIPSE>(d, vec(dir).rescale(1), cutoff, vec(center).mul(invradius), radius, orient, dist, &nodes[0], bo, br);
    return dist > -1e9f;
}

bool BIH::boxcollide(physent *d, const vec &dir, float cutoff, const vec &o, int yaw, int pitch, int roll, float scale)
{
    if(!numnodes) return false;

    vec center(d->o.x, d->o.y, d->o.z + 0.5f*(d->aboveeye - d->eyeheight)),
        radius(d->xradius, d->yradius, 0.5f*(d->eyeheight + d->aboveeye));
    center.sub(o);
    if(scale != 1) { scale = 1/scale; center.mul(scale); radius.mul(scale); }

    if(center.magnitude() >= radius.magnitude() + BIH::radius) return false;

    matrix3x3 orient;
    orient.identity();
    if(yaw) orient.rotate_around_z(sincosmod360(yaw));
    if(pitch) orient.rotate_around_x(sincosmod360(pitch));
    if(roll) orient.rotate_around_y(sincosmod360(-roll));

    vec bo = orient.transposedtransform(center), br = orient.abstransposedtransform(vec(d->radius, d->radius, radius.z));
    if(bo.x + br.x < bbmin.x || bo.y + br.y < bbmin.y || bo.z + br.z < bbmin.z ||
       bo.x - br.x > bbmax.x || bo.y - br.y > bbmax.y || bo.z - br.z > bbmax.z)
        return false;

    matrix3x3 dorient;
    dorient.setyaw(d->yaw*RAD);
    orient.mul(dorient);

    wall = vec(0, 0, 0);
    float dist = -1e10f;
    collide<COLLIDE_OBB>(d, dorient.transform(dir).rescale(1), cutoff, center, radius, matrix3x4(orient, dorient.transform(center).neg()), dist, &nodes[0], bo, br);
    if(dist > -1e9f)
    {
        wall = dorient.transposedtransform(wall);
        return true;
    }
    return false;
}

