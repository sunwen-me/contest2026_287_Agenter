#pragma once

#include "x86_lio_slam/backend/linear_solver.h"

#include <Eigen/Core>

#include <cstddef>
#include <vector>

namespace x86_lio_slam {

// A square sparse matrix whose scalar blocks are always 6x6. The graph
// backend uses one block per pose variable, so no scalar-index bookkeeping is
// needed inside the PCG multiply or preconditioner.
class BlockSparseMatrix6 final {
public:
    using Block = Eigen::Matrix<double, 6, 6>;

    struct Entry {
        std::size_t column = 0;
        Block value = Block::Zero();
    };

    BlockSparseMatrix6() = default;

    explicit BlockSparseMatrix6(std::size_t block_rows,
                                std::size_t block_columns = 0);

    std::size_t BlockRows() const { return block_rows_; }
    std::size_t BlockColumns() const { return block_columns_; }
    std::size_t ScalarRows() const { return block_rows_ * 6; }
    std::size_t ScalarColumns() const { return block_columns_ * 6; }

    void AddBlock(std::size_t row, std::size_t column, const Block& value);

    void AddScalar(std::size_t row, std::size_t column, double value);

    // AddBlock/AddScalar may be called repeatedly while assembling a matrix.
    // Finalize sorts rows and merges duplicate block entries.
    void Finalize();

    Eigen::VectorXd Multiply(const Eigen::VectorXd& vector) const;

    Eigen::MatrixXd ToDense() const;

    Block DiagonalBlock(std::size_t row) const;

    std::size_t NonzeroBlocks() const;

    static BlockSparseMatrix6 FromDense(const Eigen::MatrixXd& matrix,
                                        double drop_tolerance = 0.0);

    static BlockSparseMatrix6 FromSparse(
        const Eigen::SparseMatrix<double>& matrix,
        double drop_tolerance = 0.0);

    const std::vector<Entry>& Row(std::size_t row) const { return rows_.at(row); }

private:
    std::size_t block_rows_ = 0;
    std::size_t block_columns_ = 0;
    std::vector<std::vector<Entry>> rows_;
    bool finalized_ = false;
};

struct BlockPcgOptions {
    std::size_t max_iterations = 200;
    double relative_tolerance = 1e-8;
    double absolute_tolerance = 1e-10;
    double diagonal_regularization = 1e-12;
};

struct BlockPcgResult {
    bool converged = false;
    std::size_t iterations = 0;
    double initial_residual = 0.0;
    double final_residual = 0.0;
};

class ScalarBlockPcgSolver final : public ILinearSolverBackend {
public:
    explicit ScalarBlockPcgSolver(BlockPcgOptions options = {})
        : options_(options) {}

    bool AnalyzePattern(const Eigen::SparseMatrix<double>& hessian) override;

    bool Factorize(const Eigen::SparseMatrix<double>& hessian) override;

    bool Solve(const Eigen::VectorXd& rhs,
               Eigen::VectorXd& solution) override;

    BlockPcgResult Solve(const BlockSparseMatrix6& matrix,
                         const Eigen::VectorXd& rhs,
                         Eigen::VectorXd& solution) const;

    const BlockPcgResult& LastResult() const { return last_result_; }

private:
    BlockPcgOptions options_;
    BlockSparseMatrix6 matrix_;
    Eigen::Index analyzed_rows_ = 0;
    Eigen::Index analyzed_columns_ = 0;
    bool pattern_analyzed_ = false;
    bool factorized_ = false;
    BlockPcgResult last_result_;
};

}  // namespace x86_lio_slam
