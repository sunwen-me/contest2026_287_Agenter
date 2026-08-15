#pragma once

#include <Eigen/Core>
#include <Eigen/SparseCore>

namespace x86_lio_slam {

// The graph backend owns the factorization policy. This narrow contract lets
// direct sparse and future block-PCG implementations share the same caller.
class ILinearSolverBackend {
public:
    virtual ~ILinearSolverBackend() = default;

    virtual bool AnalyzePattern(
        const Eigen::SparseMatrix<double>& hessian) = 0;

    virtual bool Factorize(const Eigen::SparseMatrix<double>& hessian) = 0;

    virtual bool Solve(const Eigen::VectorXd& rhs,
                       Eigen::VectorXd& solution) = 0;
};

}  // namespace x86_lio_slam
