#include "DonTopo/Core/Blend2D.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace DonTopo
{
    namespace
    {
        const float kRepetido = 1e-5f;

        double orient(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c)
        {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        }

        std::vector<int> unicos(const std::vector<glm::vec2>& pts)
        {
            std::vector<int> u;
            for (int i = 0; i < (int)pts.size(); i++)
            {
                bool rep = false;
                for (int j : u)
                    if (glm::distance(pts[i], pts[j]) < kRepetido) { rep = true; break; }
                if (!rep) u.push_back(i);
            }
            return u;
        }

        // d estrictamente dentro del círculo de (a, b, c), CCW. Tolerancia
        // relativa a la escala: los concíclicos (det ~ 0) cuentan como fuera.
        bool dentroCirculo(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c, glm::dvec2 d, double escala4)
        {
            const double adx = a.x - d.x, ady = a.y - d.y;
            const double bdx = b.x - d.x, bdy = b.y - d.y;
            const double cdx = c.x - d.x, cdy = c.y - d.y;
            const double det = (adx * adx + ady * ady) * (bdx * cdy - cdx * bdy)
                             - (bdx * bdx + bdy * bdy) * (adx * cdy - cdx * ady)
                             + (cdx * cdx + cdy * cdy) * (adx * bdy - bdx * ady);
            return det > 1e-9 * escala4;
        }

        // Interiores disjuntos por ejes separadores (las 6 aristas). Tocarse en
        // una arista o un vértice NO es solaparse.
        bool solapan(const glm::dvec2 t[3], const glm::dvec2 u[3], double eps)
        {
            auto separa = [&](const glm::dvec2 a[3], const glm::dvec2 b[3]) {
                for (int e = 0; e < 3; e++)
                {
                    const glm::dvec2 p = a[e], q = a[(e + 1) % 3];
                    bool todosFuera = true;
                    for (int k = 0; k < 3; k++)
                        if (orient(p, q, b[k]) > eps) { todosFuera = false; break; }
                    if (todosFuera) return true;
                }
                return false;
            };
            return !separa(t, u) && !separa(u, t);
        }

        // Punto más cercano a p en el segmento ab: parámetro en [0, 1].
        float paramSegmento(glm::vec2 a, glm::vec2 b, glm::vec2 p)
        {
            const glm::vec2 ab = b - a;
            const float l2 = glm::dot(ab, ab);
            return l2 > 0.0f ? glm::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
        }

        int emitir(Blend2DWeight out[3], const int idx[3], const float w[3], int n)
        {
            float total = 0.0f;
            for (int i = 0; i < n; i++) total += std::max(0.0f, w[i]);
            int m = 0;
            for (int i = 0; i < n; i++)
            {
                const float wi = total > 0.0f ? std::max(0.0f, w[i]) / total : 0.0f;
                if (wi > 0.0f) out[m++] = { idx[i], wi };
            }
            return m;
        }
    }

    std::vector<Blend2DTriangle> triangulate2D(const std::vector<glm::vec2>& pts)
    {
        std::vector<Blend2DTriangle> out;
        const std::vector<int> u = unicos(pts);
        if (u.size() < 3) return out;

        glm::dvec2 lo(pts[u[0]]), hi(pts[u[0]]);
        for (int i : u) { lo = glm::min(lo, glm::dvec2(pts[i])); hi = glm::max(hi, glm::dvec2(pts[i])); }
        const double escala  = std::max(hi.x - lo.x, hi.y - lo.y);
        const double escala2 = escala * escala;
        const double epsArea = 1e-9 * escala2;

        std::vector<std::array<glm::dvec2, 3>> aceptados;
        const int n = (int)u.size();
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                for (int k = j + 1; k < n; k++)
                {
                    int ia = u[i], ib = u[j], ic = u[k];
                    glm::dvec2 a(pts[ia]), b(pts[ib]), c(pts[ic]);
                    const double o = orient(a, b, c);
                    if (std::fabs(o) <= epsArea) continue;
                    if (o < 0.0) { std::swap(ib, ic); std::swap(b, c); }
                    bool delaunay = true;
                    for (int m = 0; m < n && delaunay; m++)
                    {
                        if (m == i || m == j || m == k) continue;
                        if (dentroCirculo(a, b, c, glm::dvec2(pts[u[m]]), escala2 * escala2)) delaunay = false;
                    }
                    if (!delaunay) continue;
                    const glm::dvec2 t[3] = { a, b, c };
                    bool libre = true;
                    for (const auto& ac : aceptados)
                        if (solapan(t, ac.data(), epsArea)) { libre = false; break; }
                    if (!libre) continue;
                    aceptados.push_back({ a, b, c });
                    out.push_back({ ia, ib, ic });
                }
        return out;
    }

    int blend2DWeights(const std::vector<glm::vec2>& pts, glm::vec2 p, Blend2DWeight out[3])
    {
        const std::vector<int> u = unicos(pts);
        if (u.empty()) return 0;
        if (u.size() == 1) { out[0] = { u[0], 1.0f }; return 1; }

        const std::vector<Blend2DTriangle> tris = triangulate2D(pts);
        if (!tris.empty())
        {
            // Dentro (o en el borde) de un triángulo: baricéntricas.
            for (const auto& t : tris)
            {
                const glm::dvec2 a(pts[t.a]), b(pts[t.b]), c(pts[t.c]), q(p);
                const double area = orient(a, b, c);
                const float w[3] = { (float)(orient(q, b, c) / area),
                                     (float)(orient(a, q, c) / area),
                                     (float)(orient(a, b, q) / area) };
                if (w[0] >= -1e-5f && w[1] >= -1e-5f && w[2] >= -1e-5f)
                {
                    const int idx[3] = { t.a, t.b, t.c };
                    return emitir(out, idx, w, 3);
                }
            }
            // Fuera: el punto más cercano de las aristas (el primero en empate).
            float mejorD = INFINITY; int ia = 0, ib = 0; float mejorT = 0.0f;
            for (const auto& t : tris)
            {
                const int v[3] = { t.a, t.b, t.c };
                for (int e = 0; e < 3; e++)
                {
                    const int a = v[e], b = v[(e + 1) % 3];
                    const float s = paramSegmento(pts[a], pts[b], p);
                    const float d = glm::distance(p, glm::mix(pts[a], pts[b], s));
                    if (d < mejorD - 1e-6f) { mejorD = d; ia = a; ib = b; mejorT = s; }
                }
            }
            const int idx[2] = { ia, ib };
            const float w[2] = { 1.0f - mejorT, mejorT };
            return emitir(out, idx, w, 2);
        }

        // Sin triángulos: todos alineados (o 2 puntos). Orden a lo largo de la
        // recta de los dos más alejados, y el segmento consecutivo más cercano.
        int ea = u[0], eb = u[1]; float lejos = -1.0f;
        for (size_t i = 0; i < u.size(); i++)
            for (size_t j = i + 1; j < u.size(); j++)
            {
                const float d = glm::distance(pts[u[i]], pts[u[j]]);
                if (d > lejos) { lejos = d; ea = u[i]; eb = u[j]; }
            }
        const glm::vec2 dir = pts[eb] - pts[ea];
        std::vector<int> orden = u;
        std::stable_sort(orden.begin(), orden.end(), [&](int x, int y) {
            return glm::dot(pts[x] - pts[ea], dir) < glm::dot(pts[y] - pts[ea], dir);
        });
        float mejorD = INFINITY; int ia = orden[0], ib = orden[1]; float mejorT = 0.0f;
        for (size_t i = 0; i + 1 < orden.size(); i++)
        {
            const float s = paramSegmento(pts[orden[i]], pts[orden[i + 1]], p);
            const float d = glm::distance(p, glm::mix(pts[orden[i]], pts[orden[i + 1]], s));
            if (d < mejorD - 1e-6f) { mejorD = d; ia = orden[i]; ib = orden[i + 1]; mejorT = s; }
        }
        const int idx[2] = { ia, ib };
        const float w[2] = { 1.0f - mejorT, mejorT };
        return emitir(out, idx, w, 2);
    }
}
